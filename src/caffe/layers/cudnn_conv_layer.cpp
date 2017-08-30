#ifdef USE_CUDNN
#include <algorithm>
#include <vector>
#include <boost/tokenizer.hpp>
#include <cudnn.h>

#include "caffe/filler.hpp"
#include "caffe/layers/cudnn_conv_layer.hpp"
#include "caffe/solver.hpp"

namespace caffe {

#if !CUDNN_VERSION_MIN(6, 0, 0)
#define CUDNN_CONVOLUTION_FWD_ALGO_COUNT \
    (CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED + 1)
#define CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT \
    (CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED + 1)
#define CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT \
    (CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED + 1)
#endif

template <typename Dtype>
float gb_round2(Dtype val) {
  return std::round(val * 1.e-7) * 0.01F;
}

template <typename Dtype>
float f_round2(Dtype val) {
  return std::round(val * 100.F) * 0.01F;
}

template <typename Dtype>
void createFilterDesc(cudnnFilterDescriptor_t* desc, int n, int c, int h, int w) {
  CUDNN_CHECK(cudnnCreateFilterDescriptor(desc));
  CUDNN_CHECK(cudnnSetFilter4dDescriptor(*desc, cudnn::dataType<Dtype>::type,
      CUDNN_TENSOR_NCHW, n, c, h, w));
}

void setConvolutionDesc(Type math, cudnnConvolutionDescriptor_t conv,
    int pad_h, int pad_w, int stride_h, int stride_w) {
  int padA[2] = {pad_h, pad_w};
  int strideA[2] = {stride_h, stride_w};
  int upscaleA[2] = {1, 1};
  CUDNN_CHECK(cudnnSetConvolutionNdDescriptor(conv,
      2, padA, strideA, upscaleA, CUDNN_CROSS_CORRELATION,
      cudnn::conv_type(math)));
}

void setConvolutionDescMath(Type math, cudnnConvolutionDescriptor_t conv) {
  int padA[2];
  int strideA[2];
  int upscaleA[2];
  int arrayLengthRequested = 2;
  int arrayLength;
  cudnnConvolutionMode_t mode;
  cudnnDataType_t dataType;

  CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(conv,
      arrayLengthRequested, &arrayLength, padA, strideA, upscaleA,
      &mode, &dataType));
  CUDNN_CHECK(cudnnSetConvolutionNdDescriptor(conv,
      2, padA, strideA, upscaleA, mode,
      cudnn::conv_type(math)));
}

cudnnDataType_t convolutionDescDataType(cudnnConvolutionDescriptor_t conv) {
  int padA[2];
  int strideA[2];
  int upscaleA[2];
  int arrayLengthRequested = 2;
  int arrayLength;
  cudnnConvolutionMode_t mode;
  cudnnDataType_t dataType;

  CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(conv,
      arrayLengthRequested, &arrayLength, padA, strideA, upscaleA,
      &mode, &dataType));
  return dataType;
}

/**
 * TODO(dox) explain cuDNN interface
 */
template <typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::LayerSetUp(
    const vector<Blob*>& bottom, const vector<Blob*>& top) {
  ConvolutionLayer<Ftype, Btype>::LayerSetUp(bottom, top);
  // Initialize algorithm arrays
  fwd_algo_.resize(bottom.size());
  bwd_filter_algo_.resize(bottom.size());
  bwd_data_algo_.resize(bottom.size());

#if CUDNN_VERSION_MIN(7, 0, 0)
  cudnn_math_override_ = this->layer_param().cudnn_math_override();
  fwd_cudnn_math_.resize(bottom.size());
  bwd_filter_cudnn_math_.resize(bottom.size());
  bwd_data_cudnn_math_.resize(bottom.size());
#endif

  // initialize size arrays
  workspace_fwd_sizes_.resize(bottom.size());
  workspace_bwd_filter_sizes_.resize(bottom.size());
  workspace_bwd_data_sizes_.resize(bottom.size());

  std::string conv_algos_override = this->layer_param().convolution_param().conv_algos_override();
  boost::char_separator<char> sep(", ");
  boost::tokenizer<boost::char_separator<char>> tokens(conv_algos_override, sep);
  for (const auto& t : tokens) {
    user_algos_override_.push_back(boost::lexical_cast<int>(t));
  }
  std::string param_err = "conv_algos_override parameter vaue '" +
      conv_algos_override + "' is ill formatted";
  CHECK_EQ(3, user_algos_override_.size()) << param_err;
  if (user_algos_override_[0] >= 0) {
    CHECK_LT(user_algos_override_[0], CUDNN_CONVOLUTION_FWD_ALGO_COUNT) << param_err;
  }
  if (user_algos_override_[1] >= 0) {
    CHECK_LT(user_algos_override_[1], CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT) << param_err;
  }
  if (user_algos_override_[2] >= 0) {
    CHECK_LT(user_algos_override_[2], CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT) << param_err;
  }

  // Initializing algorithms and workspaces
  // Do not rely on initialized algorithms (Reshape will set algorithms
  // with correct values in the first iteration).
  for (size_t i = 0; i < bottom.size(); ++i) {
    fwd_algo_[i] = (cudnnConvolutionFwdAlgo_t)
        (user_algos_override_[0] >= 0 ? user_algos_override_[0] :
        CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM);
    bwd_data_algo_[i] = (cudnnConvolutionBwdDataAlgo_t)
        (user_algos_override_[1] >= 0 ? user_algos_override_[1] :
        CUDNN_CONVOLUTION_BWD_DATA_ALGO_1);
    bwd_filter_algo_[i] = (cudnnConvolutionBwdFilterAlgo_t)
        (user_algos_override_[2] >= 0 ? user_algos_override_[2] :
        CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1);

    workspace_fwd_sizes_[i] = 0;
    workspace_bwd_data_sizes_[i] = 0;
    workspace_bwd_filter_sizes_[i] = 0;

#if CUDNN_VERSION_MIN(7, 0, 0)
    fwd_cudnn_math_[i] = CUDNN_DEFAULT_MATH;
    bwd_filter_cudnn_math_[i] = CUDNN_DEFAULT_MATH;
    bwd_data_cudnn_math_[i] = CUDNN_DEFAULT_MATH;
#endif
  }
  forward_math_ = this->layer_param().forward_math();
  backward_data_math_ = backward_filter_math_ = this->layer_param().backward_math();

  // Set the indexing parameters.
  bias_offset_ = this->num_output_ / groups();

  // Create filter descriptor.
  const int* kernel_shape_data = this->kernel_shape_.cpu_data();
  const int kernel_h = kernel_shape_data[0];
  const int kernel_w = kernel_shape_data[1];

  if (use_v7grouping()) {
    createFilterDesc<Ftype>(&fwd_filter_desc_,
        this->num_output_, this->channels_ / groups(),
        kernel_h, kernel_w);
    createFilterDesc<Btype>(&bwd_filter_desc_,
        this->num_output_, this->channels_ / groups(),
        kernel_h, kernel_w);
    this->weight_offset_ = this->num_output_ *
                           (this->channels_ / groups()) * kernel_h * kernel_w;
  } else {
    createFilterDesc<Ftype>(&fwd_filter_desc_,
        this->num_output_ / groups(), this->channels_ / groups(),
        kernel_h, kernel_w);
    createFilterDesc<Btype>(&bwd_filter_desc_,
        this->num_output_ / groups(), this->channels_ / groups(),
        kernel_h, kernel_w);
    this->weight_offset_ = (this->num_output_ / groups()) *
                           (this->channels_ / groups()) * kernel_h * kernel_w;
  }

  // Create tensor descriptor(s) for data and corresponding convolution(s).
  for (int i = 0; i < bottom.size(); i++) {
    cudnnTensorDescriptor_t fwd_bottom_desc, bwd_bottom_desc;
    cudnn::createTensor4dDesc<Ftype>(&fwd_bottom_desc);
    cudnn::createTensor4dDesc<Btype>(&bwd_bottom_desc);
    fwd_bottom_descs_.push_back(fwd_bottom_desc);
    bwd_bottom_descs_.push_back(bwd_bottom_desc);
    cudnnTensorDescriptor_t fwd_top_desc, bwd_top_desc;
    cudnn::createTensor4dDesc<Ftype>(&fwd_top_desc);
    cudnn::createTensor4dDesc<Btype>(&bwd_top_desc);
    fwd_top_descs_.push_back(fwd_top_desc);
    bwd_top_descs_.push_back(bwd_top_desc);
    cudnnConvolutionDescriptor_t fwd_conv_desc, bwd_conv_data_desc, bwd_conv_filter_desc;
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&fwd_conv_desc));
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&bwd_conv_data_desc));
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&bwd_conv_filter_desc));
#ifdef CUDNN_GROUPING
    if (use_v7grouping()) {
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(fwd_conv_desc, groups()));
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(bwd_conv_data_desc, groups()));
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(bwd_conv_filter_desc, groups()));
    }
#endif

    fwd_conv_descs_.push_back(fwd_conv_desc);
    bwd_conv_data_descs_.push_back(bwd_conv_data_desc);
    bwd_conv_filter_descs_.push_back(bwd_conv_filter_desc);

    cudnnTensorDescriptor_t fwd_cached_bottom_desc;
    cudnn::createTensor4dDesc<Ftype>(&fwd_cached_bottom_desc);
    fwd_cached_bottom_descs_.push_back(fwd_cached_bottom_desc);
    cudnnTensorDescriptor_t bwd_cached_bottom_desc;
    cudnn::createTensor4dDesc<Btype>(&bwd_cached_bottom_desc);
    bwd_cached_bottom_descs_.push_back(bwd_cached_bottom_desc);

    cudnnConvolutionDescriptor_t fwd_cached_conv_desc;
    cudnnConvolutionDescriptor_t bwd_cached_conv_data_desc, bwd_cached_conv_filter_desc;
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&fwd_cached_conv_desc));
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&bwd_cached_conv_data_desc));
    CUDNN_CHECK(cudnnCreateConvolutionDescriptor(&bwd_cached_conv_filter_desc));
#ifdef CUDNN_GROUPING
    if (use_v7grouping()) {
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(fwd_cached_conv_desc, groups()));
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(bwd_cached_conv_data_desc, groups()));
      CUDNN_CHECK(cudnnSetConvolutionGroupCount(bwd_cached_conv_filter_desc, groups()));
    }
#endif
    fwd_cached_conv_descs_.push_back(fwd_cached_conv_desc);
    bwd_cached_conv_data_descs_.push_back(bwd_cached_conv_data_desc);
    bwd_cached_conv_filter_descs_.push_back(bwd_cached_conv_filter_desc);
  }

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::createTensor4dDesc<Ftype>(&fwd_bias_desc_);
    cudnn::createTensor4dDesc<Btype>(&bwd_bias_desc_);
  }

  handles_setup_ = true;
  // When true, Reshape asks cuDNN (either Get ot FindEx) for the best algorithm
  use_algo_seeker_ = true;
  // When true, a small amount of workspace is allowed for algorithms
  use_modest_workspace_ = true;
  // When true, Reshape sets descriptors, algorithms, workspaces.
  use_reshape_ = true;
  // When true, cached bottom and conv descriptors need to be set.
  initialized_cached_descs_ = false;
}

template <typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::AllocateFindExWorkspace() {
  const int dev = Caffe::current_device();
  if (was_reduced(dev)) {
    return;
  }
  GPUMemory::Workspace& ws = workspace(dev);
  // Try to use the amount estimated for all groups
  size_t req_bytes = mem_size_estimated(dev);
  if (req_bytes <= ws.size()) {
    return;  // job is done by previous layer on this GPU
  }
  const size_t to_reserve = PAGE_SIZE + align_up<7>(this->weight_offset_) * sizeof(Btype);

  size_t bytes_available, bytes_total;
  GPUMemory::GetInfo(&bytes_available, &bytes_total, true);
  size_t bytes_possible = ws.size() + bytes_available;
  bytes_possible = align_down<7>(bytes_possible > to_reserve ? bytes_possible - to_reserve : 0UL);

  if (req_bytes > bytes_possible) {
    LOG(WARNING) << " " << this->print_current_device()
        << " Avail " << gb_round2(bytes_available) << "G"
        << " + Curr " << gb_round2(ws.size()) << "G,"
        << " Estimate " << gb_round2(req_bytes) << "G";
    req_bytes = bytes_possible;
  }
  if (req_bytes <= ws.size()) {
    return;
  }
  int attempts = ATTEMPTS_TO_RESERVE_WS;
  while (!ws.try_reserve(req_bytes) && attempts > 0) {
    req_bytes = align_down<7>(req_bytes > PAGE_SIZE ? req_bytes - PAGE_SIZE : 0UL);
    --attempts;
    LOG(INFO) << this->print_current_device() << " Retrying to allocate " << req_bytes
              << " bytes, attempts left: " << attempts;
  }
}

template <typename Ftype, typename Btype>
size_t CuDNNConvolutionLayer<Ftype, Btype>::AllocateWorkspace(size_t bottom_size) {
  const int dev = Caffe::current_device();
  for (int i = 0; i < bottom_size; ++i) {
    if (this->phase_ == TRAIN) {
      // get workspace for backwards data algorithm
      CUDNN_CHECK(cudnnGetConvolutionBackwardDataWorkspaceSize(Caffe::cudnn_handle(),
          bwd_filter_desc_, bwd_top_descs_[i], bwd_conv_data_descs_[i], bwd_bottom_descs_[i],
          bwd_data_algo_[i], &workspace_bwd_data_sizes_[i]));
      // get workspace for backwards filter algorithm
      CUDNN_CHECK(cudnnGetConvolutionBackwardFilterWorkspaceSize(Caffe::cudnn_handle(),
          bwd_bottom_descs_[i], bwd_top_descs_[i], bwd_conv_filter_descs_[i], bwd_filter_desc_,
          bwd_filter_algo_[i], &workspace_bwd_filter_sizes_[i]));
    }
    // get workspace for forwards
    CUDNN_CHECK(cudnnGetConvolutionForwardWorkspaceSize(Caffe::cudnn_handle(),
        fwd_bottom_descs_[i], fwd_filter_desc_, fwd_conv_descs_[i], fwd_top_descs_[i],
        fwd_algo_[i], &(workspace_fwd_sizes_[i])));
  }
  CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));

  for (int i = 0; i < bottom_size; ++i) {
    setmax_mem_req_all_grps(align_up<7>(workspace_fwd_sizes_[i]) * ws_groups(),
        dev, this->phase_);
    if (this->phase_ == TRAIN) {
      setmax_mem_req_all_grps(align_up<7>(workspace_bwd_data_sizes_[i]) * ws_groups(),
          dev, this->phase_);
      setmax_mem_req_all_grps(align_up<7>(workspace_bwd_filter_sizes_[i]) * ws_groups(),
          dev, this->phase_);
    }
  }
  GPUMemory::Workspace& ws = workspace(dev);
  ws.safe_reserve(mem_req_all_grps(dev, this->phase_));
  return ws.size();
}

template <typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::Reshape(
    const vector<Blob*>& bottom, const vector<Blob*>& top) {
  // Check whether cached descriptors have been initialized.
  if (initialized_cached_descs_) {
    // Check whether bottom and conv descriptors have changed,
    // which then requires a new reshape and set algo.
    if (IsBottomDescChanged(bottom, true) || IsBottomDescChanged(bottom, false) ||
        IsConvDescChanged(bottom, true) || IsConvDescChanged(bottom, false)) {
      use_reshape_ = true;
      // When reshape, algorithms need to be set again.
      use_algo_seeker_ = true;
      use_modest_workspace_ = true;
    } else {
      // When no reshape is needed, setting algo may be still needed
      // (for example, if we are at iteration 1).
      // If we want to set algos, we have to use reshape in
      // current implementation.
      use_reshape_ = use_algo_seeker_;
    }
  } else {
    // If cached descriptors are not initialized yet, need to
    // do reshape which also initializes cached descriptors.
    use_reshape_ = true;
  }

  if (!use_algo_seeker_ && !use_modest_workspace_ && this->phase_ == TRAIN) {
    const int dev = Caffe::current_device();
    if (!was_reduced(dev) && mem_size_estimated(dev) > 0UL) {
      GPUMemory::Workspace& ws = workspace(dev);
      size_t mem_req = 2UL * std::max(mem_req_all_grps(dev, TRAIN), mem_req_all_grps(dev, TEST));
      if (mem_req > 0UL && ws.size() > mem_req) {
        // Winner needs less than half of initial estimate - saving the rest
        // Half because we want to reduce the number of allocs/deallocs
        LOG(INFO) << this->print_current_device()
                  << " Layer '" << this->name() << "' reallocating workspace "
                  << gb_round2(ws.size()) << "G to " << gb_round2(mem_req) << "G";
        // TRAIN only
        ws.release();
        ws.reserve(mem_req);
        was_reduced(dev) = true;
      }
    }
  }

  if (!use_reshape_) {
    return;
  }

  ConvolutionLayer<Ftype, Btype>::Reshape(bottom, top);
  CHECK_EQ(2, this->num_spatial_axes_)
      << "CuDNNConvolution input must have 2 spatial axes "
      << "(e.g., height and width). "
      << "Use 'engine: CAFFE' for general ND convolution.";

  bottom_offset_ = this->bottom_dim_ / groups();
  top_offset_ = this->top_dim_ / groups();

  const int height = bottom[0]->shape(this->channel_axis_ + 1);
  const int width = bottom[0]->shape(this->channel_axis_ + 2);
  const int height_out = top[0]->shape(this->channel_axis_ + 1);
  const int width_out = top[0]->shape(this->channel_axis_ + 2);
  const int* pad_data = this->pad_.cpu_data();
  const int pad_h = pad_data[0];
  const int pad_w = pad_data[1];
  const int* stride_data = this->stride_.cpu_data();
  const int stride_h = stride_data[0];
  const int stride_w = stride_data[1];

    // Set cuDNN tensor and convolution descriptors
  for (int i = 0; i < bottom.size(); i++) {
    cudnn::setTensor4dDesc<Ftype>(&fwd_bottom_descs_[i],
        this->num_,
        use_v7grouping() ? this->channels_ : this->channels_ / groups(),
        height, width,
        this->channels_ * height * width,
        height * width, width, 1);
    cudnn::setTensor4dDesc<Btype>(&bwd_bottom_descs_[i],
        this->num_,
        use_v7grouping() ? this->channels_ : this->channels_ / groups(),
        height, width,
        this->channels_ * height * width,
        height * width, width, 1);
    cudnn::setTensor4dDesc<Ftype>(&fwd_top_descs_[i],
        this->num_,
        use_v7grouping() ? this->num_output_ : this->num_output_ / groups(),
        height_out, width_out,
        this->num_output_ * this->out_spatial_dim_,
        this->out_spatial_dim_, width_out, 1);
    cudnn::setTensor4dDesc<Btype>(&bwd_top_descs_[i],
        this->num_,
        use_v7grouping() ? this->num_output_ : this->num_output_ / groups(),
        height_out, width_out,
        this->num_output_ * this->out_spatial_dim_,
        this->out_spatial_dim_, width_out, 1);

    setConvolutionDesc(forward_math_, fwd_conv_descs_[i],
        pad_h, pad_w, stride_h, stride_w);
    setConvolutionDesc(forward_math_, fwd_cached_conv_descs_[i],
        pad_h, pad_w, stride_h, stride_w);
    setConvolutionDesc(backward_data_math_, bwd_conv_data_descs_[i],
        pad_h, pad_w, stride_h, stride_w);
    setConvolutionDesc(backward_filter_math_, bwd_conv_filter_descs_[i],
        pad_h, pad_w, stride_h, stride_w);
    setConvolutionDesc(backward_data_math_, bwd_cached_conv_data_descs_[i],
        pad_h, pad_w, stride_h, stride_w);
    setConvolutionDesc(backward_filter_math_, bwd_cached_conv_filter_descs_[i],
        pad_h, pad_w, stride_h, stride_w);

    // Set cached descriptors
    cudnn::setTensor4dDesc<Ftype>(&fwd_cached_bottom_descs_[i],
        this->num_,
        use_v7grouping() ? this->channels_ : this->channels_ / groups(),
        height, width,
        this->channels_ * height * width,
        height * width, width, 1);
    cudnn::setTensor4dDesc<Btype>(&bwd_cached_bottom_descs_[i],
        this->num_,
        use_v7grouping() ? this->channels_ : this->channels_ / groups(),
        height, width,
        this->channels_ * height * width,
        height * width, width, 1);
  }
  initialized_cached_descs_ = true;

  // Tensor descriptor for bias.
  if (this->bias_term_) {
    cudnn::setTensor4dDesc<Ftype>(&fwd_bias_desc_, 1,
        use_v7grouping() ? this->num_output_ : this->num_output_ / groups(),
        1, 1);
    cudnn::setTensor4dDesc<Btype>(&bwd_bias_desc_, 1,
        use_v7grouping() ? this->num_output_ : this->num_output_ / groups(),
        1, 1);
  }

  // Ask cuDNN to find the best algorithm
  // When batch is small and every image is different we don't want to call Find* over and over
  if (use_algo_seeker_) {
    // FindEx: A workspace of size workspace_bytes is allocated for FindEx.
    //         Besides, workspace, a buffer is allocated for the output of
    //         FindEx-backward-filter. The size of buffer is as big as weights.
    // Get: workspace_bytes is only used as a workspace limit by Get.
    //      (no allocation happens before Get or by Get).
    size_t workspace_bytes = 0UL;
    if (use_modest_workspace_) {
      // In iteration 0, use a small amount of memory in order to leave
      // most of memory for allocating layer blobs.
      workspace_bytes = AllocateWorkspace(bottom.size());
    } else {
      // Avoid seeking for an algorithm in subsequent iterations
      use_algo_seeker_ = false;
    }
    switch (this->layer_param_.convolution_param().cudnn_convolution_algo_seeker()) {
      case ConvolutionParameter_CuDNNConvolutionAlgorithmSeeker_GET:
        GetConvAlgo(bottom, top, workspace_bytes, pad_h, pad_w, stride_h, stride_w);
        AllocateWorkspace(bottom.size());
        break;
      case ConvolutionParameter_CuDNNConvolutionAlgorithmSeeker_FINDEX:
        if (!use_modest_workspace_) {
          if (this->phase_ == TRAIN) {
            // Make sure it's all allocated before we take the rest
            for (int i = 0; i < bottom.size(); ++i) {
              top[i]->allocate_data();
              bottom[i]->allocate_diff();
            }
            this->blobs_[0]->allocate_data();

            EstimateMaxWorkspaceSize(bottom, top);
            AllocateFindExWorkspace();
          }
          // Also used by Test Net but based on shared space taken by Train:
          FindExConvAlgo(bottom, top);

          if (this->phase_ == TEST) {
            use_algo_seeker_ = false;
          }
        }
        break;
      default:
        LOG(FATAL) << "Wrong value for cudnn_convolution_algo_seeker";
    }
  }
}

template <typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::EstimateMaxWorkspaceSize(const vector<Blob*>& bottom,
    const vector<Blob*>& top) {
  cudnnStatus_t status;
  size_t size;
  size_t available_memory, total_memory;
  const int dev = Caffe::current_device();
#if CUDNN_VERSION_MIN(7, 0, 0)
  // does it support TENSOR_OP?
  const bool top_device = Caffe::device_capability(dev) >= 700;
#endif
  GPUMemory::GetInfo(&available_memory, &total_memory, true);
  std::list<int> algos_to_test;
  for (int i = 0; i < bottom.size(); ++i) {
    if (user_algos_override_[0] < 0) {
      for (int a = 0; a < CUDNN_CONVOLUTION_FWD_ALGO_COUNT; ++a) {
        algos_to_test.emplace_back(a);
      }
    } else {
      algos_to_test.emplace_back(user_algos_override_[0]);
    }
    for (int a : algos_to_test) {
#if CUDNN_VERSION_MIN(7, 0, 0)
      for (int cm = 0; cm < 2; ++cm) {
        if (top_device) {
          CUDNN_CHECK(cudnnSetConvolutionMathType(fwd_conv_descs_[i],
              cm == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH));
        } else if (cm > 0) {
          break;
        }
#endif
        for (int m = 0; m < 2; ++m) {
        if (m == 1) {
          if (is_type<Ftype>(FLOAT16) &&
              convolutionDescDataType(fwd_conv_descs_[i]) == CUDNN_DATA_HALF) {
            // second run in pseudo fp32 mode
            setConvolutionDescMath(FLOAT, fwd_conv_descs_[i]);
          } else {
            continue;
          }
        }
        // get workspace for forwards
        status = cudnnGetConvolutionForwardWorkspaceSize(Caffe::cudnn_handle(),
            fwd_bottom_descs_[i], fwd_filter_desc_, fwd_conv_descs_[i], fwd_top_descs_[i],
            (cudnnConvolutionFwdAlgo_t) a, &size);
        CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
        if (status == CUDNN_STATUS_SUCCESS) {
          size *= ws_groups();
          if (size < total_memory) {
            setmax_mem_size_estimated(size, dev);
          }
        }
        if (m == 1) {
          setConvolutionDescMath(FLOAT16, fwd_conv_descs_[i]);
        }
      }
#if CUDNN_VERSION_MIN(7, 0, 0)
      }
#endif
    }

    algos_to_test.clear();
    if (user_algos_override_[1] < 0) {
      for (int a = 0; a < CUDNN_CONVOLUTION_BWD_DATA_ALGO_COUNT; ++a) {
        algos_to_test.emplace_back(a);
      }
    } else {
      algos_to_test.emplace_back(user_algos_override_[1]);
    }
    for (int a : algos_to_test) {
#if CUDNN_VERSION_MIN(7, 0, 0)
      for (int cm = 0; cm < 2; ++cm) {
        if (top_device) {
          CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_data_descs_[i],
              cm == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH));
        } else if (cm > 0) {
          break;
        }
#endif
        for (int m = 0; m < 2; ++m) {
        if (m == 1) {
          if (is_type<Ftype>(FLOAT16) &&
              convolutionDescDataType(bwd_conv_data_descs_[i]) == CUDNN_DATA_HALF) {
            // second run in pseudo fp32 mode
            setConvolutionDescMath(FLOAT, bwd_conv_data_descs_[i]);
          } else {
            continue;
          }
        }
        // get workspace for backwards data algorithm
        status = cudnnGetConvolutionBackwardDataWorkspaceSize(Caffe::cudnn_handle(),
            bwd_filter_desc_, bwd_top_descs_[i], bwd_conv_data_descs_[i], bwd_bottom_descs_[i],
            (cudnnConvolutionBwdDataAlgo_t) a, &size);
        CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
        if (status == CUDNN_STATUS_SUCCESS) {
          size *= ws_groups();
          if (size < total_memory) {
            setmax_mem_size_estimated(size, dev);
          }
        }
        if (m == 1) {
          setConvolutionDescMath(FLOAT16, bwd_conv_data_descs_[i]);
        }
      }
#if CUDNN_VERSION_MIN(7, 0, 0)
      }
#endif
    }

    algos_to_test.clear();
    if (user_algos_override_[2] < 0) {
      for (int a = 0; a < CUDNN_CONVOLUTION_BWD_FILTER_ALGO_COUNT; ++a) {
        algos_to_test.emplace_back(a);
      }
    } else {
      algos_to_test.emplace_back(user_algos_override_[2]);
    }
    for (int a : algos_to_test) {
#if CUDNN_VERSION_MIN(7, 0, 0)
      for (int cm = 0; cm < 2; ++cm) {
        if (top_device) {
          CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_filter_descs_[i],
              cm == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH));
        } else if (cm > 0) {
          break;
        }
#endif
        for (int m = 0; m < 2; ++m) {
        if (m == 1) {
          if (is_type<Ftype>(FLOAT16) &&
              convolutionDescDataType(bwd_conv_filter_descs_[i]) == CUDNN_DATA_HALF) {
            // second run in pseudo fp32 mode
            setConvolutionDescMath(FLOAT, bwd_conv_filter_descs_[i]);
          } else {
            continue;
          }
        }
        // get workspace for backwards filter algorithm
        status = cudnnGetConvolutionBackwardFilterWorkspaceSize(Caffe::cudnn_handle(),
            bwd_bottom_descs_[i], bwd_top_descs_[i], bwd_conv_filter_descs_[i], bwd_filter_desc_,
            (cudnnConvolutionBwdFilterAlgo_t) a, &size);
        CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
        if (status == CUDNN_STATUS_SUCCESS) {
          size *= ws_groups();
          if (size < total_memory) {
            setmax_mem_size_estimated(size, dev);
          }
        }
        if (m == 1) {
          setConvolutionDescMath(FLOAT16, bwd_conv_filter_descs_[i]);
        }
      }
#if CUDNN_VERSION_MIN(7, 0, 0)
      }
#endif
    }

#if CUDNN_VERSION_MIN(7, 0, 0)
    if (top_device) {
      // Restore current values
      CUDNN_CHECK(cudnnSetConvolutionMathType(fwd_conv_descs_[i],
          fwd_cudnn_math_[i]));
      CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_filter_descs_[i],
          bwd_filter_cudnn_math_[i]));
      CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_data_descs_[i],
          bwd_data_cudnn_math_[i]));
    }
#endif
  }
}

template <typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::GetConvAlgo(const vector<Blob*>& bottom,
    const vector<Blob*>& top, const size_t workspace_bytes, int pad_h, int pad_w,
    int stride_h, int stride_w) {
  for (int i = 0; i < bottom.size(); ++i) {
    // Get backward data algorithm (if not set by user)
    if (user_algos_override_[1] < 0) {
      CUDNN_CHECK(cudnnGetConvolutionBackwardDataAlgorithm(Caffe::cudnn_handle(),
          bwd_filter_desc_, bwd_top_descs_[i], bwd_conv_data_descs_[i], bwd_bottom_descs_[i],
          CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
          align_down<7>(workspace_bytes / ws_groups()), &bwd_data_algo_[i]));
    }
    // Get forward algorithm (if not set by user)
    if (user_algos_override_[0] < 0) {
      CUDNN_CHECK(cudnnGetConvolutionForwardAlgorithm(Caffe::cudnn_handle(),
          fwd_bottom_descs_[i], fwd_filter_desc_, fwd_conv_descs_[i], fwd_top_descs_[i],
          CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
          align_down<7>(workspace_bytes / ws_groups()), &fwd_algo_[i]));
      CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
    }
    // Get backward filter algorithm (if not set by user)
    if (user_algos_override_[2] < 0) {
      CUDNN_CHECK(cudnnGetConvolutionBackwardFilterAlgorithm(Caffe::cudnn_handle(),
          bwd_bottom_descs_[i], bwd_top_descs_[i], bwd_conv_filter_descs_[i], bwd_filter_desc_,
          CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
          align_down<7>(workspace_bytes / ws_groups()), &bwd_filter_algo_[i]));
      CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
    }
    LOG(INFO) << Phase_Name(this->phase_)
        << " Conv Algos by Get* (F,BD,BF) for layer '" << this->name()
        << "' with space " << workspace_bytes << "/" << ws_groups() <<  " "
        << fwd_algo_[i] << " " << bwd_data_algo_[i] << " " << bwd_filter_algo_[i];
  }
}

template<typename Ftype, typename Btype>
void CuDNNConvolutionLayer<Ftype, Btype>::FindExConvAlgo(
    const vector<Blob*>& bottom, const vector<Blob*>& top) {
  int fwd_algo_count = 0;
  int filter_algo_count = 0;
  int data_algo_count = 0;
  cudnnConvolutionFwdAlgoPerf_t fwd_results[REQUEST_ALGO_COUNT];
  cudnnConvolutionBwdFilterAlgoPerf_t bwd_filter_results[REQUEST_ALGO_COUNT];
  cudnnConvolutionBwdDataAlgoPerf_t bwd_data_results[REQUEST_ALGO_COUNT];
  bool fwd_pseudo = false;
  bool bwd_filter_pseudo = false;
  bool bwd_data_pseudo = false;
  float ftime = 0.F, bdtime = 0.F, bftime = 0.F;

  cudnnHandle_t handle = Caffe::cudnn_handle();
  cudaStream_t stream;
  CUDNN_CHECK(cudnnGetStream(handle, &stream));
  CHECK(stream == Caffe::thread_stream());

#if CUDNN_VERSION_MIN(7, 0, 0)
  // does it support TENSOR_OP?
  const bool top_device = Caffe::device_capability(Caffe::current_device()) >= 700;
  bool try_top = top_device && !use_modest_workspace_;
  if (cudnn_math_override_ < 0 && (is_precise<Ftype>() || is_precise<Btype>())) {
    // 32/64 mode, user doesn't override => default math only
    try_top = false;
  }
#endif

  const int dev = Caffe::current_device();
  GPUMemory::Workspace& ws = workspace(dev);
  GPUMemory::Workspace& tmp_ws = tmp_weights(dev);

  const size_t gsize = ws.size() / ws_groups();
  CHECK(is_even(gsize)) << ws.size() << " / " << ws_groups() << " -> " << gsize;

  if (this->phase_ == TRAIN) {
    // Allocate temporary buffer for weights used for backward filter FindEx
    const size_t tmp_weights_size = align_up<7>(this->weight_offset_) * sizeof(Btype);
    tmp_ws.safe_reserve(tmp_weights_size);
  }

  for (int i = 0; i < bottom.size(); ++i) {
#if CUDNN_VERSION_MIN(7, 0, 0)
    cudnnMathType_t fwd_cudnn_math_0 = CUDNN_DEFAULT_MATH;
    if (try_top) {
      fwd_cudnn_math_[i] = fwd_cudnn_math_0 =
          cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
      CUDNN_CHECK(cudnnSetConvolutionMathType(fwd_conv_descs_[i], fwd_cudnn_math_[i]));
    }
#endif
    // Find forward algorithm
    if (user_algos_override_[0] < 0) {
      float algo_time = 0.F;
      for (int m = 0; m < 2; ++m) {
        if (m > 0 &&
            // if user wants specific math type, no need to check anything else
            (this->is_fm_by_user() ||
             // also, we skip this in fp32/64 modes
             !is_type<Ftype>(FLOAT16) ||
             // and we skip 1st cycle
             use_modest_workspace_ ||
             // and sanity check for current descriptor type
             convolutionDescDataType(fwd_conv_descs_[i]) != CUDNN_DATA_HALF)) {
          break;
        }
        if (m == 1) {
          // second run in pseudo fp32 mode
          setConvolutionDescMath(FLOAT, fwd_conv_descs_[i]);
        }

        int prev_algo = -1;
        for (int t = 0; t < 5; ++t) {
          CUDNN_CHECK(cudnnFindConvolutionForwardAlgorithmEx(handle,
              fwd_bottom_descs_[i],
              bottom[i]->gpu_data<Ftype>(),
              fwd_filter_desc_,
              this->blobs_[0]->template gpu_data<Ftype>(),
              fwd_conv_descs_[i],
              fwd_top_descs_[i],
              top[i]->mutable_gpu_data<Ftype>(),  // overwritten
              REQUEST_ALGO_COUNT,
              &fwd_algo_count,
              fwd_results,
              ws.data(),
              gsize));
          CUDA_CHECK(cudaStreamSynchronize(stream));
          // Waiting for two identical decisions in a row
          if (prev_algo == (int)fwd_results[0].algo) {
            break;
          }
          prev_algo = (int)fwd_results[0].algo;
        }

        for (int k = 0; k < fwd_algo_count; ++k) {
          if (fwd_results[k].status == CUDNN_STATUS_SUCCESS) {
            if (m == 0) {
              algo_time = fwd_results[k].time;
            } else {
              // here we compare pseudo fp32 against native fp16
              if (fwd_results[k].time >= algo_time) {
                // pseudo fp32 lost, switching back to native fp16
                setConvolutionDescMath(FLOAT16, fwd_conv_descs_[i]);
                break;
              }
              // pseudo fp32 won
              forward_math_ = tpm(tp<Ftype>(), FLOAT);
            }
            fwd_algo_[i] = fwd_results[k].algo;
#if CUDNN_VERSION_MIN(7, 0, 0)
            if (cudnn_math_override_ < 0) {
              // Winning Math for either native or pseudo mode:
              fwd_cudnn_math_0 = fwd_results[k].mathType;
            } else {
              fwd_cudnn_math_0 =
                  cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
            }
#endif
            workspace_fwd_sizes_[i] = align_up<7>(fwd_results[k].memory);
            setmax_mem_req_all_grps(workspace_fwd_sizes_[i] * ws_groups(), dev, this->phase_);
            fwd_pseudo = is_precise(forward_math_) && !is_precise(tp<Ftype>());
            break;
          }
        }
      }
    }
#if CUDNN_VERSION_MIN(7, 0, 0)
    if (top_device && !use_modest_workspace_) {
      fwd_cudnn_math_[i] = fwd_cudnn_math_0;
      CUDNN_CHECK(cudnnSetConvolutionMathType(fwd_conv_descs_[i], fwd_cudnn_math_[i]));
    }
#endif

    // Only set backward-filter/data algorithms in training phase
    if (this->phase_ == TRAIN) {
#if CUDNN_VERSION_MIN(7, 0, 0)
      cudnnMathType_t bwd_filter_cudnn_math_0 = CUDNN_DEFAULT_MATH;
      if (try_top) {
        bwd_filter_cudnn_math_[i] = bwd_filter_cudnn_math_0 =
            cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
        CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_filter_descs_[i],
            bwd_filter_cudnn_math_[i]));
      }
#endif
      if (user_algos_override_[2] < 0) {
        float algo_time = 0.F;
        for (int m = 0; m < 2; ++m) {
          if (m > 0 &&
              // if user wants specific math type, no need to check anything else
              (this->is_bm_by_user() ||
               // also, we skip this in fp32/64 modes
               !is_type<Ftype>(FLOAT16) ||
               // and we skip 1st cycle
               use_modest_workspace_ ||
               // and sanity check for current descriptor type
               convolutionDescDataType(bwd_conv_filter_descs_[i])
               != CUDNN_DATA_HALF)) {
            break;
          }
          if (m == 1) {
            // second run in pseudo fp32 mode
            setConvolutionDescMath(FLOAT, bwd_conv_filter_descs_[i]);
          }

          int prev_algo = -1;
          for (int t = 0; t < 5; ++t) {
            // Find backward filter algorithm
            CUDNN_CHECK(cudnnFindConvolutionBackwardFilterAlgorithmEx(handle,
                bwd_bottom_descs_[i],
                bottom[i]->gpu_data<Btype>(),
                bwd_top_descs_[i],
                top[i]->gpu_diff<Btype>(),
                bwd_conv_filter_descs_[i],
                bwd_filter_desc_,
                tmp_ws.data(),  // overwritten
                REQUEST_ALGO_COUNT,
                &filter_algo_count,
                bwd_filter_results,
                ws.data(),
                gsize));
            CUDA_CHECK(cudaStreamSynchronize(stream));
            // Waiting for two identical decisions in a row
            if (prev_algo == (int)bwd_filter_results[0].algo) {
              break;
            }
            prev_algo = (int)bwd_filter_results[0].algo;
          }

          for (int k = 0; k < filter_algo_count; ++k) {
            if (bwd_filter_results[k].status == CUDNN_STATUS_SUCCESS) {
              if (m == 0) {
                algo_time = bwd_filter_results[k].time;
              } else {
                // here we compare pseudo fp32 against native fp16
                if (bwd_filter_results[k].time >= algo_time) {
                  // pseudo fp32 lost, switching back to native fp16
                  setConvolutionDescMath(FLOAT16, bwd_conv_filter_descs_[i]);
                  break;
                }
                // pseudo fp32 won
                backward_filter_math_ = tpm(tp<Btype>(), FLOAT);
              }
              bwd_filter_algo_[i] = bwd_filter_results[k].algo;
#if CUDNN_VERSION_MIN(7, 0, 0)
              if (cudnn_math_override_ < 0) {
                // Winning Math for either native or pseudo mode:
                bwd_filter_cudnn_math_0 = bwd_filter_results[k].mathType;
              } else {
                bwd_filter_cudnn_math_0 =
                    cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
              }
#endif
              workspace_bwd_filter_sizes_[i] = align_up<7>(bwd_filter_results[k].memory);
              setmax_mem_req_all_grps(workspace_bwd_filter_sizes_[i] * ws_groups(),
                  dev, this->phase_);
              bwd_filter_pseudo = is_precise(backward_filter_math_) && !is_precise(tp<Btype>());
              bftime = bwd_filter_results[k].time;
              break;
            }
          }
        }
      }
#if CUDNN_VERSION_MIN(7, 0, 0)
      if (top_device && !use_modest_workspace_) {
        bwd_filter_cudnn_math_[i] = bwd_filter_cudnn_math_0;
        CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_filter_descs_[i],
            bwd_filter_cudnn_math_[i]));
      }

      cudnnMathType_t bwd_data_cudnn_math_0 = CUDNN_DEFAULT_MATH;
      if (try_top && cudnn_math_override_ != 0) {
        bwd_data_cudnn_math_[i] = bwd_data_cudnn_math_0 =
            cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
        CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_data_descs_[i],
            bwd_data_cudnn_math_[i]));
      }
#endif
      if (user_algos_override_[1] < 0) {
        float algo_time = 0.F;
        for (int m = 0; m < 2; ++m) {
          if (m > 0 &&
              // if user wants specific math type, no need to check anything else
              (this->is_bm_by_user() ||
               // also, we skip this in fp32/64 modes
               !is_type<Ftype>(FLOAT16) ||
               // and we skip 1st cycle
               use_modest_workspace_ ||
               // and sanity check for current descriptor type
               convolutionDescDataType(bwd_conv_data_descs_[i])
               != CUDNN_DATA_HALF)) {
            break;
          }
          if (m == 1) {
            // second run in pseudo fp32 mode
            setConvolutionDescMath(FLOAT, bwd_conv_data_descs_[i]);
          }

          int prev_algo = -1;
          for (int t = 0; t < 5; ++t) {
            // Find backward data algorithm
            CUDNN_CHECK(cudnnFindConvolutionBackwardDataAlgorithmEx(handle,
                bwd_filter_desc_,
                this->blobs_[0]->template gpu_data<Btype>(),
                bwd_top_descs_[i],
                top[i]->gpu_diff<Btype>(),
                bwd_conv_data_descs_[i],
                bwd_bottom_descs_[i],
                bottom[i]->mutable_gpu_diff<Btype>(),  // overwritten
                REQUEST_ALGO_COUNT,
                &data_algo_count,
                bwd_data_results,
                ws.data(),
                gsize));
            CUDA_CHECK(cudaStreamSynchronize(stream));
            // Waiting for two identical decisions in a row
            if (prev_algo == (int)bwd_data_results[0].algo) {
              break;
            }
            prev_algo = (int)bwd_data_results[0].algo;
          }

          for (int k = 0; k < data_algo_count; ++k) {
            if (bwd_data_results[k].status == CUDNN_STATUS_SUCCESS) {
              if (m == 0) {
                algo_time = bwd_data_results[k].time;
              } else {
                // here we compare pseudo fp32 against native fp16
                if (bwd_data_results[k].time >= algo_time) {
                  // pseudo fp32 lost, switching back to native fp16
                  setConvolutionDescMath(FLOAT16, bwd_conv_data_descs_[i]);
                  break;
                }
                // pseudo fp32 won
                backward_data_math_ = tpm(tp<Btype>(), FLOAT);
              }
              bwd_data_algo_[i] = bwd_data_results[k].algo;
#if CUDNN_VERSION_MIN(7, 0, 0)
              if (cudnn_math_override_ < 0) {
                // Winning Math for either native or pseudo mode:
                bwd_data_cudnn_math_0 = bwd_data_results[k].mathType;
              } else {
                bwd_data_cudnn_math_0 =
                    cudnn_math_override_ == 0 ? CUDNN_DEFAULT_MATH : CUDNN_TENSOR_OP_MATH;
              }
#endif
              workspace_bwd_data_sizes_[i] = align_up<7>(bwd_data_results[k].memory);
              setmax_mem_req_all_grps(workspace_bwd_data_sizes_[i] * ws_groups(),
                  dev, this->phase_);
              bwd_data_pseudo = is_precise(backward_data_math_) && !is_precise(tp<Btype>());
              bdtime = bwd_data_results[k].time;
              break;
            }
          }
        }
      }
#if CUDNN_VERSION_MIN(7, 0, 0)
      if (top_device && !use_modest_workspace_) {
        bwd_data_cudnn_math_[i] = bwd_data_cudnn_math_0;
        CUDNN_CHECK(cudnnSetConvolutionMathType(bwd_conv_data_descs_[i],
            bwd_data_cudnn_math_[i]));
      }
#endif
    }

    CUDA_CHECK(cudaStreamSynchronize(Caffe::thread_stream()));
    size_t available_memory, total_memory;
    GPUMemory::GetInfo(&available_memory, &total_memory, true);
    std::ostringstream os;
    os << this->print_current_device()
        << (this->phase_ == TRAIN ? " Conv Algos (F,BD,BF): '" : " Conv Algo (F): '")
        << this->name() << "' with space "
        << gb_round2(ws.size()) << "G " << this->channels_ << "/" << this->group_
        << (use_v7grouping() ? "." : "")
#ifdef DEBUG
        << " -> [" << workspace_fwd_sizes_[i]
        << " " << workspace_bwd_data_sizes_[i]
        << " " << workspace_bwd_filter_sizes_[i] << "]"
#endif
        << " " << fwd_algo_[i]
#if CUDNN_VERSION_MIN(7, 0, 0)
        << (fwd_cudnn_math_[i] == CUDNN_TENSOR_OP_MATH ? "T" : "")
#endif
        << (user_algos_override_[0] >= 0 ? "u " : (fwd_pseudo ? "p " : " "));

    if (this->phase_ == TRAIN) {
      os << bwd_data_algo_[i]
#if CUDNN_VERSION_MIN(7, 0, 0)
          << (bwd_data_cudnn_math_[i] == CUDNN_TENSOR_OP_MATH ? "T" : "")
#endif
          << (user_algos_override_[1] >= 0 ? "u " : (bwd_data_pseudo ? "p " : " "))
          << bwd_filter_algo_[i]
#if CUDNN_VERSION_MIN(7, 0, 0)
          << (bwd_filter_cudnn_math_[i] == CUDNN_TENSOR_OP_MATH ? "T" : "")
#endif
          << (user_algos_override_[2] >= 0 ? "u " : (bwd_filter_pseudo ? "p " : " "));
    }

    os << " (avail " << gb_round2(available_memory) << "G, req "
       << gb_round2(mem_req_all_grps(dev, this->phase_)) << "G)\tt: " << f_round2(ftime);

    if (this->phase_ == TRAIN) {
      os << " " << f_round2(bdtime) << " " << f_round2(bftime);
    }

    LOG(INFO) << os.str();
  }
}

// Checked if there is a difference between the corresponding descriptors in
// cached_bottom_descs_ and bottom_descs_.
// No need to compare all parameters: batchsize, height, and width are enough.
template <typename Ftype, typename Btype>
bool CuDNNConvolutionLayer<Ftype, Btype>::IsBottomDescChanged(
  const vector<Blob*>& bottom, bool fwd_mode) {
  int cached_n; int cached_c; int cached_h; int cached_w;
  int cached_stride_n; int cached_stride_c;
  int cached_stride_h; int cached_stride_w;
  int n; int c; int h; int w;
  cudnnDataType_t type;

  for (int i = 0; i < bottom.size(); i++) {
    CUDNN_CHECK(cudnnGetTensor4dDescriptor(
        fwd_mode ? fwd_cached_bottom_descs_[i] : bwd_cached_bottom_descs_[i],
        &type,
        &cached_n, &cached_c, &cached_h, &cached_w,
        &cached_stride_n, &cached_stride_c,
        &cached_stride_h, &cached_stride_w));
    const vector<int>& shape = bottom[i]->shape();
    n = shape[0];
    c = shape[1] / ws_groups();
    h = shape[2];
    w = shape[3];

    if ((cached_n != n) || (cached_c != c) || (cached_h != h) || (cached_w != w)) {
      return true;
    }
  }
  return false;
}

// Checked if there is a difference between the corresponding descriptors in
// cached_conv_descs_ and conv_descs_.
// No need to compare all parameters; pads, strides, and upscales are enough.
template <typename Ftype, typename Btype>
bool CuDNNConvolutionLayer<Ftype, Btype>::IsConvDescChanged(
  const vector<Blob*>& bottom, bool fwd_mode) {
  int cached_padA[2];
  int padA[2];
  int cached_strideA[2];
  int strideA[2];
  int cached_upscaleA[2];
  int upscaleA[2];
  int arrayLength;
  cudnnConvolutionMode_t mode;
  cudnnDataType_t type;

  for (int i = 0; i < bottom.size(); i++) {
    CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(
        fwd_mode ? fwd_cached_conv_descs_[i] : bwd_cached_conv_data_descs_[i],
        2, &arrayLength, cached_padA, cached_strideA, cached_upscaleA,
        &mode, &type));
    CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(
        fwd_mode ? fwd_conv_descs_[i] : bwd_conv_data_descs_[i],
        2, &arrayLength, padA, strideA, upscaleA, &mode, &type));
    if ((cached_padA[0] != padA[0]) ||
        (cached_padA[1] != padA[1]) ||
        (cached_strideA[0]  != strideA[0])  ||
        (cached_strideA[1]  != strideA[1])  ||
        (cached_upscaleA[0] != upscaleA[0]) ||
        (cached_upscaleA[1] != upscaleA[1])) {
      return true;
    }
    if (!fwd_mode) {
      CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(
        bwd_cached_conv_filter_descs_[i],
        2, &arrayLength, cached_padA, cached_strideA, cached_upscaleA,
        &mode, &type));
      CUDNN_CHECK(cudnnGetConvolutionNdDescriptor(
        bwd_conv_filter_descs_[i],
        2, &arrayLength, padA, strideA, upscaleA, &mode, &type));
      if ((cached_padA[0] != padA[0]) ||
          (cached_padA[1] != padA[1]) ||
          (cached_strideA[0]  != strideA[0])  ||
          (cached_strideA[1]  != strideA[1])  ||
          (cached_upscaleA[0] != upscaleA[0]) ||
          (cached_upscaleA[1] != upscaleA[1])) {
        return true;
      }
    }
  }
  return false;
}

template <typename Ftype, typename Btype>
CuDNNConvolutionLayer<Ftype, Btype>::~CuDNNConvolutionLayer() {
  // Check that handles have been setup before destroying.
  if (!handles_setup_) { return; }

  for (int i = 0; i < fwd_bottom_descs_.size(); ++i) {
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(fwd_bottom_descs_[i]));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bwd_bottom_descs_[i]));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(fwd_top_descs_[i]));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bwd_top_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(fwd_conv_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(bwd_conv_data_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(bwd_conv_filter_descs_[i]));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(fwd_cached_bottom_descs_[i]));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bwd_cached_bottom_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(fwd_cached_conv_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(bwd_cached_conv_data_descs_[i]));
    CUDNN_CHECK(cudnnDestroyConvolutionDescriptor(bwd_cached_conv_filter_descs_[i]));
  }
  if (this->bias_term_) {
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(fwd_bias_desc_));
    CUDNN_CHECK(cudnnDestroyTensorDescriptor(bwd_bias_desc_));
  }
  CUDNN_CHECK(cudnnDestroyFilterDescriptor(fwd_filter_desc_));
  CUDNN_CHECK(cudnnDestroyFilterDescriptor(bwd_filter_desc_));
}

INSTANTIATE_CLASS_FB(CuDNNConvolutionLayer);

}   // namespace caffe
#endif
