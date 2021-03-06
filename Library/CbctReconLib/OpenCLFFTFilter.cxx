// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++, C#, and Java:
// http://www.viva64.com

#include "OpenCLFFTFilter.h"

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#ifdef USE_CLFFT
#include <clFFT.h>
#endif // USE_CLFFT

#include "rtkOpenCLUtilities.h"

std::tuple<cl_platform_id, cl_device_id>
getPlatformAndDeviceID(const size_t required_mem_alloc_size) {
  cl_platform_id platform;
  cl_device_id device;

  /* Setup OpenCL environment. */
  auto err = clGetPlatformIDs(1, &platform, nullptr);

  err |= clGetDeviceIDs(platform, CL_DEVICE_TYPE_DEFAULT, 1, &device, nullptr);

  cl_ulong max_mem_alloc;
  err |= clGetDeviceInfo(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong),
                         &max_mem_alloc, nullptr);

  if (required_mem_alloc_size > max_mem_alloc) {
    std::cout << "Memory required: " << required_mem_alloc_size / (1024 * 1024)
              << "MB"
              << " was larger than DEV_MAX_MEM: "
              << max_mem_alloc / (1024 * 1024) << "MB\n"
              << "Changing to CPU mode..." << std::endl;
    err |= clGetDeviceIDs(platform, CL_DEVICE_TYPE_CPU, 1, &device, nullptr);
  }
  if (err != CL_SUCCESS) {
    std::cout << "Could not retrieve OpenCL device info, error code: " << err
              << std::endl;
  }

  return std::make_tuple(platform, device);
}

std::tuple<cl_context, cl_command_queue, cl_device_id>
get_constext_queue(const size_t required_mem_alloc_size) {
  auto err = CL_SUCCESS;
  auto plat_dev = getPlatformAndDeviceID(required_mem_alloc_size);

  cl_context_properties props[3] = {
      CL_CONTEXT_PLATFORM,
      reinterpret_cast<cl_context_properties>(std::get<0>(plat_dev)), 0};

  const auto ctx = clCreateContext(&props[0], 1, &std::get<1>(plat_dev),
                                   nullptr, nullptr, &err);
  if (err != CL_SUCCESS) {
    std::cout << "Could not create OpenCL context, error code: " << err
              << std::endl;
  }
  const auto queue = clCreateCommandQueue(ctx, std::get<1>(plat_dev), 0, &err);
  if (err != CL_SUCCESS) {
    std::cout << "Could not create OpenCL queue, error code: " << err
              << std::endl;
  }
  return std::make_tuple(ctx, queue, std::get<1>(plat_dev));
}

void release_ctx_queue(cl_context ctx, cl_command_queue queue) {
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);
}

template <int N, typename T> void set_kernel_arg(cl_kernel &kernel, T &value) {
  cl_int err = clSetKernelArg(kernel, N, sizeof(T), &value);
  if (err != CL_SUCCESS) {
    std::cout << "Could not parse OpenCL argument" << N
              << ", error code: " << err << std::endl;
  }
}
template <int N, typename T>
void set_kernel_mem_arg(cl_kernel &kernel, T *value) {
  auto err = clSetKernelArg(kernel, N, sizeof(cl_mem), (void *)&value);
  if (err != CL_SUCCESS) {
    std::cout << "Could not parse OpenCL cl_mem argument" << N
              << ", error code: " << err << std::endl;
  }
}

size_t get_local_work_size_small(cl_device_id device) {
  char device_name[128];
  const auto err = clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(device_name),
                                   &device_name, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "Could not read device info, error code: " << err << std::endl;
  }
  if (strcmp(device_name, "Intel(R) Iris(TM) Pro Graphics 5200") == 0) {
    return 16;
  }
  return 128;
}
size_t get_local_work_size_large(cl_device_id device) {
  char device_name[128];
  const auto err = clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(device_name),
                                   &device_name, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "Could not read device info, error code: " << err << std::endl;
  }
  if (strcmp(device_name, "Intel(R) Iris(TM) Pro Graphics 5200") == 0) {
    return 32;
  }
  return 128;
}

void OpenCL_padding(const cl_int4 &paddingIndex, const cl_uint4 &paddingSize,
                    const cl_uint4 &inputSize,
                    const float *hostVolume, // input
                    float *hostPaddedVolume, // output
                    const std::vector<float> &mirrorWeights) {
  auto err = CL_SUCCESS;
  cl_program m_Program;

  const auto pv_buffer_size =
      paddingSize.x * paddingSize.y * paddingSize.z * sizeof(float);
  const auto ctx_queue_dev = get_constext_queue(pv_buffer_size);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);
  /* Prepare OpenCL memory objects and place data inside them. */
  const auto w_buffer_size = mirrorWeights.size() * sizeof(float);
  const auto w_buf_sizeof = static_cast<cl_uint>(
      mirrorWeights
          .size()); // just because intellisense couldn't understand it below...
  auto weights_d =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                     w_buffer_size, (void *)&mirrorWeights[0], &err);
  // err = clEnqueueWriteBuffer(queue, weights_d, CL_TRUE, 0, w_buffer_size,
  // &(mirrorWeights[0]), 0, NULL, NULL);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not write OpenCL weigths_d buffer, error code: "
              << err << std::endl;
  }

  auto devicePaddedVolume =
      clCreateBuffer(ctx, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                     pv_buffer_size, (void *)&hostPaddedVolume[0], &err);

  if (err != CL_SUCCESS) {
    std::cout
        << "PAD::Could not write OpenCL devicePaddedVolume buffer, error code: "
        << err << std::endl;
  }

  const auto v_buffer_size = sizeof(hostVolume);
  auto deviceVolume =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, v_buffer_size,
                     (void *)&hostVolume[0], &err);
  // err = clEnqueueWriteBuffer(queue, deviceVolume, CL_TRUE, 0, v_buffer_size,
  // hostVolume, 0, NULL, NULL);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not write OpenCL deviceVolume buffer, error code: "
              << err << std::endl;
  }

  // some text
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "padding_kernel", &err);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  auto local_work_size = get_local_work_size_small(device);

  const size_t global_work_size = paddingSize.x * paddingSize.y * paddingSize.z;

  set_kernel_mem_arg<0>(m_Kernel, deviceVolume);
  set_kernel_mem_arg<1>(m_Kernel, devicePaddedVolume);
  set_kernel_arg<2>(m_Kernel, paddingIndex);
  set_kernel_arg<3>(m_Kernel, paddingSize);
  set_kernel_arg<4>(m_Kernel, inputSize);
  set_kernel_mem_arg<5>(m_Kernel, weights_d);
  set_kernel_arg<6>(m_Kernel, w_buf_sizeof);

  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not parse OpenCL arguments, error code: " << err
              << std::endl;
  }

  // Call to kernel

  // Execute kernel
  cl_event events[2];
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not wait for OpenCL event!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err =
      clEnqueueReadBuffer(queue, devicePaddedVolume, CL_TRUE, 0, pv_buffer_size,
                          hostPaddedVolume, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "PAD::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL memory objects. */
  clReleaseMemObject(weights_d);
  clReleaseMemObject(deviceVolume);
  clReleaseMemObject(devicePaddedVolume);

  /* Release OpenCL working objects. */
  release_ctx_queue(ctx, queue);
  // std::cout << "Did it work? we just don't know..." << std::endl;
}

#ifdef USE_CLFFT

template <typename T>
void clFFT_Forward_Multiply_Backward(T *hostProjection,
                                     const std::complex<T> *hostKernelFFT,
                                     const cl_int4 inputDimension,
                                     const cl_int2 kernelDimension) {
  if (!std::is_floating_point<T>::value)
    throw std::invalid_argument("Projection data is not floating point data");

  cl_int err;
  cl_context_properties props[3] = {CL_CONTEXT_PLATFORM, 0, 0};
  cl_context ctx = 0;
  cl_command_queue queue = 0;

  /* FFT library realted declarations */
  clfftPlanHandle planHandle;
  clfftDim dim = CLFFT_3D;

  const size_t sizeProjection =
      inputDimension.x * inputDimension.y * inputDimension.z;
  const size_t memorySizeProjection = sizeProjection * sizeof(T);
  cl_int4 fftDimension;
  fftDimension.x = inputDimension.x / 2 + 1;
  fftDimension.y = inputDimension.y;
  fftDimension.z = inputDimension.z;
  const size_t sizeFFTProjection =
      fftDimension.x * fftDimension.y * fftDimension.z;
  const size_t memorySizeFFTProjection =
      sizeFFTProjection * sizeof(std::complex<T>);
  std::vector<std::complex<T>> hostProjectionFFT(sizeFFTProjection);

  const auto ctx_queue_dev = get_constext_queue(memorySizeProjection);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  /* Setup clFFT. */
  clfftSetupData fftSetupFwd;
  err = clfftInitSetupData(&fftSetupFwd);
  fftSetupFwd.debugFlags = CLFFT_DUMP_PROGRAMS;
  err |= clfftSetup(&fftSetupFwd);

  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not setup clfft, error code: " << err << std::endl;
  // device pointers
  cl_mem deviceProjection;    // copy of host
  cl_mem deviceProjectionFFT; // copy of host
  // cl_int2 fftDimension = inputDimension;
  // fftDimension.x = inputDimension.x / 2 + 1;

  // const size_t memorySizeProjectionFFT = fftDimension.x * fftDimension.y *
  // sizeof(std::complex<T>);  std::cout << "Creating buffers..." << std::endl;
  /* Prepare OpenCL memory objects and place data inside them. */
  deviceProjection =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeProjection, &hostProjection[0], &err);
  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not create OpenCL buffers 1, error code: " << err
              << std::endl;
  /*err = clEnqueueWriteBuffer(queue, deviceProjection, CL_TRUE, 0,
  memorySizeProjection, &hostProjection[0], 0, NULL, NULL); if (err !=
  CL_SUCCESS) std::cout << "FWD::Could not write OpenCL buffers 1, error code: "
  << err << std::endl;*/

  deviceProjectionFFT =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeFFTProjection, &hostProjectionFFT[0], &err);
  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not create OpenCL buffers 1, error code: " << err
              << std::endl;

  const size_t clLengths[3] = {static_cast<size_t>(inputDimension.z),
                               static_cast<size_t>(inputDimension.y),
                               static_cast<size_t>(inputDimension.x)};

  /* Create a default plan for a complex FFT. */
  err = clfftCreateDefaultPlan(&planHandle, ctx, dim, clLengths);
  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not create default plan, error code: " << err
              << std::endl;

  /* Set plan parameters. */
  if (sizeof(T) == sizeof(float))
    err = clfftSetPlanPrecision(planHandle, CLFFT_SINGLE);
  else if (sizeof(T) == sizeof(double))
    err = clfftSetPlanPrecision(planHandle, CLFFT_DOUBLE);
  else
    throw std::invalid_argument("Type of input doesn't fit in a standard "
                                "double or float (long double not possible)");

  err |= clfftSetLayout(planHandle, CLFFT_REAL, CLFFT_HERMITIAN_INTERLEAVED);
  err |= clfftSetResultLocation(planHandle, CLFFT_OUTOFPLACE);
  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not set up plan, error code: " << err << std::endl;

  /* Bake the plan. */
  err |= clfftBakePlan(planHandle, 1, &queue, NULL, NULL);

  // get the buffersize
  size_t buffersize = 0;
  err |= clfftGetTmpBufSize(planHandle, &buffersize);

  // allocate the intermediate buffer
  cl_mem clMedBuffer = NULL;

  if (buffersize) {
    clMedBuffer = clCreateBuffer(ctx, CL_MEM_READ_WRITE, buffersize, 0, &err);
    if (err != CL_SUCCESS)
      std::cout << "FWD::Could not create medBuffer, error code: " << err
                << std::endl;
  }

  /* Execute the plan. */
  err |= clfftEnqueueTransform(planHandle, CLFFT_FORWARD, 1, &queue, 0, NULL,
                               NULL, &deviceProjection, &deviceProjectionFFT,
                               clMedBuffer); // &deviceProjectionFFT, NULL);

  if (err != CL_SUCCESS)
    std::cout
        << "FWD::Could not perform forward fourier transform, error code: "
        << err << std::endl;

  /* Wait for calculations to be finished. */
  err = clFinish(queue);

  if (err != CL_SUCCESS)
    std::cout << "FWD::Could not finish OpenCL queue, error code: " << err
              << std::endl;

  const size_t memorySizeKernelFFT =
      kernelDimension.x * kernelDimension.y * sizeof(cl_float2);

  /* Prepare OpenCL memory objects and place data inside them. */
  cl_mem deviceKernelFFT =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeKernelFFT, (void *)&hostKernelFFT[0], &err);

  // err = clEnqueueWriteBuffer(queue, deviceProjectionFFT, CL_TRUE, 0,
  // memorySizeProjectionFFT, &hostProjectionFFT[0], 0, NULL, NULL);  err |=
  // clEnqueueWriteBuffer(queue, deviceKernelFFT, CL_TRUE, 0,
  // memorySizeKernelFFT, &hostKernelFFT[0], 0, NULL, NULL);

  if (err != CL_SUCCESS)
    std::cout << "MUL::Could create OpenCL buffers, error code: " << err
              << std::endl;

  // Create program
  cl_program m_Program;
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  cl_kernel m_Kernel;
  if (kernelDimension.y == 1)
    m_Kernel = clCreateKernel(m_Program, "multiply_kernel", &err);
  else
    m_Kernel = clCreateKernel(m_Program, "multiply_kernel2D", &err);

  if (err != CL_SUCCESS)
    std::cout << "MUL::Could not create OpenCL kernel, error code: " << err
              << std::endl;

  cl_event events[2];

  size_t local_work_size = get_local_work_size_large(device);

  const size_t global_work_size =
      static_cast<size_t>(fftDimension.x * fftDimension.y * fftDimension.z);

  err =
      clSetKernelArg(m_Kernel, 0, sizeof(cl_mem), (void *)&deviceProjectionFFT);
  err |= clSetKernelArg(m_Kernel, 1, sizeof(cl_int4), &fftDimension);
  err |= clSetKernelArg(m_Kernel, 2, sizeof(cl_mem), (void *)&deviceKernelFFT);
  if (err != CL_SUCCESS)
    std::cout << "MUL::Could not parse args to OpenCL kernel, error code: "
              << err << std::endl;

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, NULL, &global_work_size,
                               &local_work_size, 0, NULL, &events[0]);

  err |= clWaitForEvents(1, &events[0]);
  err |= clReleaseEvent(events[0]);
  if (err != CL_SUCCESS)
    std::cout << "MUL::Could not release OpenCL event, error code: " << err
              << std::endl;

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS)
    std::cout << "MUL::Could not finish OpenCL queue, error code: " << err
              << std::endl;

  // DONE resetting queue

  /* Setup clFFT. */
  clfftSetupData fftSetupBck;
  err = clfftInitSetupData(&fftSetupBck);
  err |= clfftSetup(&fftSetupBck);

  /* Create a default plan for a complex FFT. */
  err |= clfftCreateDefaultPlan(&planHandle, ctx, dim, clLengths);

  /* Set plan parameters. */
  if (sizeof(T) == sizeof(float))
    err = clfftSetPlanPrecision(planHandle, CLFFT_SINGLE);
  else if (sizeof(T) == sizeof(double))
    err = clfftSetPlanPrecision(planHandle, CLFFT_DOUBLE);
  else
    throw std::invalid_argument("Type of input doesn't fit in a standard "
                                "double or float (long double not possible)");

  err |= clfftSetLayout(planHandle, CLFFT_HERMITIAN_INTERLEAVED, CLFFT_REAL);
  err |= clfftSetResultLocation(planHandle, CLFFT_OUTOFPLACE);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not set OpenCL plan parameters, error code: "
              << err << std::endl;

  /* Bake the plan. */
  err = clfftBakePlan(planHandle, 1, &queue, NULL, NULL);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not bake OpenCL kernel, error code: " << err
              << std::endl;

  /* Execute the plan. */
  err = clfftEnqueueTransform(planHandle, CLFFT_BACKWARD, 1, &queue, 0, NULL,
                              NULL, &deviceProjectionFFT, &deviceProjection,
                              clMedBuffer);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not create OpenCL kernel, error code: " << err
              << std::endl;

  /* Wait for calculations to be finished. */
  err = clFinish(queue);
  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not finish OpenCL queue, error code: " << err
              << std::endl;

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceProjection, CL_TRUE, 0,
                            memorySizeProjection, &hostProjection[0], 0, NULL,
                            NULL);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not read OpenCL output, error code: " << err
              << std::endl;

  /* Release OpenCL memory objects. */
  clReleaseMemObject(deviceProjection);
  clReleaseMemObject(deviceProjectionFFT);
  clReleaseMemObject(deviceKernelFFT);

  if (clMedBuffer)
    clReleaseMemObject(clMedBuffer);

  /* Release the plan. */
  err = clfftDestroyPlan(&planHandle);

  /* Release clFFT library. */
  clfftTeardown();

  /* Release OpenCL working objects. */
  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not release OpenCL kernel, error code: " << err
              << std::endl;
}

void clFFT_backwards(float *hostProjection, const cl_float2 *hostProjectionFFT,
                     const cl_int4 inputDimension) {

  // INV FFT START
  cl_int err;
  cl_context_properties props[3] = {CL_CONTEXT_PLATFORM, 0, 0};
  cl_context ctx = 0;
  cl_command_queue queue = 0;

  /* FFT library realted declarations */
  clfftPlanHandle planHandle;
  clfftDim dim = CLFFT_3D;

  const size_t memorySizeProjection =
      inputDimension.x * inputDimension.y * inputDimension.z * sizeof(float);
  const auto ctx_queue_dev = get_constext_queue(memorySizeProjection);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // std::cout << "Creating buffers..." << std::endl;

  cl_int4 fftDimension = inputDimension;
  fftDimension.x = inputDimension.x / 2 + 1;

  const size_t memorySizeProjectionFFT =
      fftDimension.x * fftDimension.y * fftDimension.z * sizeof(cl_float2);

  /* Prepare OpenCL memory objects and place data inside them. */
  cl_mem deviceProjectionFFT = clCreateBuffer(
      ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, memorySizeProjectionFFT,
      (void *)&hostProjectionFFT[0], &err);
  cl_mem deviceProjection =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeProjection, hostProjection, &err);
  /*
  err = clEnqueueWriteBuffer(queue, deviceProjectionFFT, CL_TRUE, 0,
  memorySizeProjectionFFT, hostProjectionFFT, 0, NULL, NULL); err =
  clEnqueueWriteBuffer(queue, deviceProjection,    CL_TRUE, 0,
  memorySizeProjection,    hostProjection, 0, NULL, NULL);
  */
  // std::cout << "Buffers created..." << std::endl;

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could create OpenCL buffers, error code: " << err
              << std::endl;

  size_t clLengths[3] = {(size_t)inputDimension.z, (size_t)inputDimension.y,
                         (size_t)inputDimension.x};

  // DONE resetting queue

  /* Setup clFFT. */
  clfftSetupData fftSetupBck;
  err = clfftInitSetupData(&fftSetupBck);
  err |= clfftSetup(&fftSetupBck);

  /* Create a default plan for a complex FFT. */
  err |= clfftCreateDefaultPlan(&planHandle, ctx, dim, clLengths);

  /* Set plan parameters. */
  err |= clfftSetPlanPrecision(planHandle, CLFFT_SINGLE);
  err |= clfftSetLayout(planHandle, CLFFT_HERMITIAN_INTERLEAVED, CLFFT_REAL);
  err |= clfftSetResultLocation(planHandle, CLFFT_OUTOFPLACE);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not set OpenCL plan parameters, error code: "
              << err << std::endl;

  /* Bake the plan. */
  err = clfftBakePlan(planHandle, 1, &queue, NULL, NULL);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not bake OpenCL kernel, error code: " << err
              << std::endl;

  /* Execute the plan. */
  err = clfftEnqueueTransform(planHandle, CLFFT_BACKWARD, 1, &queue, 0, NULL,
                              NULL, &deviceProjectionFFT, &deviceProjection,
                              NULL);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not create OpenCL kernel, error code: " << err
              << std::endl;

  /* Wait for calculations to be finished. */
  err = clFinish(queue);
  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not finish OpenCL queue, error code: " << err
              << std::endl;

  /* Fetch results of calculations. */
  err =
      clEnqueueReadBuffer(queue, deviceProjection, CL_TRUE, 0,
                          memorySizeProjection, hostProjection, 0, NULL, NULL);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not read OpenCL output, error code: " << err
              << std::endl;

  /* Release OpenCL memory objects. */
  clReleaseMemObject(deviceProjection);
  clReleaseMemObject(deviceProjectionFFT);

  /* Release the plan. */
  err = clfftDestroyPlan(&planHandle);

  /* Release clFFT library. */
  clfftTeardown();

  /* Release OpenCL working objects. */
  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS)
    std::cout << "BCK::Could not release OpenCL kernel, error code: " << err
              << std::endl;
}

void OpenCL_fft_convolution(const cl_int4 inputDimension,
                            const cl_int2 kernelDimension,
                            float *hostProjection,
                            std::complex<float> *hostKernelFFT) {
  // std::cout << "Forward projecting..." << std::endl;
  try {
    clFFT_Forward_Multiply_Backward<float>(hostProjection, hostKernelFFT,
                                           inputDimension, kernelDimension);
  } catch (std::exception &e) {
    std::cerr << "Exception trown: " << e.what() << std::endl;
  }

  // OpenCL_multiply_by_kernel(hostProjectionFFT, hostKernelFFT, inputDimension,
  // kernelDimension);

  // clFFT_backwards(hostProjection, hostProjectionFFT, inputDimension);

  printf(".");
}

#endif // USE_CLFFT

void OpenCL_subtract3Dfrom2DbySlice_InPlace(
    cl_float *buffer, const cl_float *sub_buffer,
    const itk::Image<float, 3U>::SizeType &inputSize,
    const itk::Image<float, 2U>::SizeType &subSize) {
  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_float);
  const auto memorySizeSub = subSize[0] * subSize[1] * sizeof(cl_float);

  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);
  auto deviceSubBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, memorySizeSub,
                     (void *)&sub_buffer[0], &err);
  /*
  err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
  buffer, 0, NULL, NULL); if (err != CL_SUCCESS) std::cout << "SUB::Could not
  create OpenCL buffers, error code: " << err << std::endl; err =
  clEnqueueWriteBuffer(queue, deviceSubBuffer, CL_TRUE, 0, memorySizeSub,
  sub_buffer, 0, NULL, NULL); if (err != CL_SUCCESS) std::cout << "SUB::Could
  not write OpenCL buffers, error code: " << err << std::endl;
          */

  // Create program
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "subtract_kernel2D", &err);

  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  cl_event events[2];

  auto local_work_size = get_local_work_size_large(device);

  const auto global_work_size = inputSize[0] * inputSize[1] * inputSize[2];

  const cl_uint4 inputDim = {{static_cast<cl_uint>(inputSize[0]),
                              static_cast<cl_uint>(inputSize[1]),
                              static_cast<cl_uint>(inputSize[2]), 0}};

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_arg<1>(m_Kernel, inputDim);
  set_kernel_mem_arg<2>(m_Kernel, deviceSubBuffer);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
                            buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceSubBuffer);
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  // std::cout << "Did it work? we just don't know..." << std::endl; IT WORKS
  if (err != CL_SUCCESS) {
    std::cout << "SUB::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }
}

itk::Image<float, 3U>::Pointer OpenCL_divide3Dby3D_OutOfPlace(
    const itk::Image<unsigned short, 3U>::Pointer &Num3D,
    const itk::Image<unsigned short, 3U>::Pointer &Denum3D) {

  const auto region = Num3D->GetLargestPossibleRegion();
  auto buffer = Num3D->GetBufferPointer();
  auto inputSize = region.GetSize();
  auto sub_buffer = Denum3D->GetBufferPointer();
  auto subSize = Denum3D->GetLargestPossibleRegion().GetSize();

  auto outImage = itk::Image<float, 3U>::New();
  const auto projCT_size = region.GetSize();
  const auto projCT_idxStart = region.GetIndex();
  const auto projCT_spacing = Num3D->GetSpacing();
  const auto projCT_origin = Num3D->GetOrigin();

  itk::Image<float, 3U>::RegionType projCT_region;
  projCT_region.SetSize(projCT_size);
  projCT_region.SetIndex(projCT_idxStart);

  outImage->SetRegions(projCT_region);
  outImage->SetSpacing(projCT_spacing);
  outImage->SetOrigin(projCT_origin);

  outImage->Allocate();

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_ushort);
  const auto memorySizeOutput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_float);
  const auto memorySizeSub =
      subSize[0] * subSize[1] * subSize[2] * sizeof(cl_ushort);

  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // std::cout << "SUB3D::Input xyz size: " << inputSize[0] * inputSize[1] *
  // inputSize[2] << std::endl; std::cout << "SUB3D::subIm xyz size: " <<
  // subSize[0] * subSize[1] * subSize[2] << std::endl;

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);
  auto deviceSubBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, memorySizeSub,
                     (void *)&sub_buffer[0], &err);
  auto deviceOutBuffer = clCreateBuffer(
      ctx, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR, memorySizeOutput,
      (void *)&outImage->GetBufferPointer()[0], &err);
  /*
  err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
  buffer, 0, NULL, NULL); err = clEnqueueWriteBuffer(queue, deviceSubBuffer,
  CL_TRUE, 0, memorySizeSub, sub_buffer, 0, NULL, NULL); err =
  clEnqueueWriteBuffer(queue, deviceOutBuffer, CL_TRUE, 0, memorySizeOutput,
  outImage->GetBufferPointer(), 0, NULL, NULL);
  */
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could create OpenCL buffers, error code: " << err
              << std::endl;
  }

  // Create program
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "divide_kernel3D_Ushort", &err);

  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  cl_event events[2];
  auto local_work_size = get_local_work_size_small(device);
  const auto global_work_size = inputSize[0] * inputSize[1] * inputSize[2];

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_mem_arg<1>(m_Kernel, deviceSubBuffer);
  set_kernel_mem_arg<2>(m_Kernel, deviceOutBuffer);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not wait for OpenCL events!?, error code: "
              << err << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */

  err = clEnqueueReadBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
                            outImage->GetBufferPointer(), 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceSubBuffer);
  clReleaseMemObject(deviceOutBuffer);
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  // std::cout << "Did it work? we just don't know..." << std::endl; IT WORKS
  if (err != CL_SUCCESS) {
    std::cout << "SUB3D::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  return outImage;
}

void OpenCL_AddConst_InPlace(cl_float *buffer,
                             const itk::Image<float, 3U>::SizeType& inputSize,
                             const cl_float constant) {

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_float);

  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // itk::Image<float, 3U>::SizeType inputSize =
  // input->GetLargestPossibleRegion().GetSize();

  // std::cout << "ADD::Input xy size: " << inputSize[0] * inputSize[1] <<
  // std::endl;

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);

  // err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0,
  // memorySizeInput, buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could create OpenCL buffers, error code: " << err
              << std::endl;
  }

  // Create program
  // std::cout << "build subtraction kernel..." << std::endl;
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "add_const_kernel", &err);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  std::cout << "Kernel created." << std::endl;
  cl_event events[2];
  auto local_work_size = get_local_work_size_large(device);

  const auto global_work_size = inputSize[0] * inputSize[1] * inputSize[2];

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_arg<1>(m_Kernel, constant);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
                            buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }
}

void OpenCL_AddConst_MulConst_InPlace(cl_float *buffer,
                                      const itk::Image<float, 3U>::SizeType& inputSize,
                                      const cl_float add_constant,
                                      const cl_float mul_constant) {

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_float);

  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);

  // err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0,
  // memorySizeInput, buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could create OpenCL buffers, error code: " << err
              << std::endl;
  }

  // Create program
  // std::cout << "build subtraction kernel..." << std::endl;
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "add_mul_const_kernel", &err);

  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  std::cout << "Kernel created." << std::endl;
  cl_event events[2];

  auto local_work_size = get_local_work_size_large(device);

  const auto global_work_size = inputSize[0] * inputSize[1] * inputSize[2];

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_arg<1>(m_Kernel, add_constant);
  set_kernel_arg<2>(m_Kernel, mul_constant);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not wait for OpenCL events!?, error code: "
              << err << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
                            buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "ADDMUL::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }
}

void OpenCL_AddConst_InPlace_2D(cl_float *buffer,
                                const itk::Image<float, 2U>::SizeType& inputSize,
                                const cl_float constant) {

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput = inputSize[0] * inputSize[1] * sizeof(cl_float);
  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // itk::Image<float, 3U>::SizeType inputSize =
  // input->GetLargestPossibleRegion().GetSize();

  // std::cout << "ADD::Input xy size: " << inputSize[0] * inputSize[1] <<
  // std::endl;

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);

  // err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0,
  // memorySizeInput, buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could create OpenCL buffers, error code: " << err
              << std::endl;
  }

  // Create program
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "add_const_kernel", &err);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  cl_event events[2];

  auto local_work_size = get_local_work_size_large(device);

  const auto global_work_size = inputSize[0] * inputSize[1];

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_arg<1>(m_Kernel, constant);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
                            buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "ADD::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }
}

cl_float2 OpenCL_min_max(const cl_float *buffer,
                         const itk::Image<float, 3U>::SizeType& inputSize) {
  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput =
      inputSize[0] * inputSize[1] * inputSize[2] * sizeof(cl_float);
  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // std::cout << "MMM::Input xy size: " << inputSize[0] * inputSize[1] *
  // inputSize[2] << std::endl;

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);
  // err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0,
  // memorySizeInput, buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could create OpenCL buffers, error code: " << err
              << std::endl;
  }

  // Create program
  // std::cout << "build min max kernel..." << std::endl;
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "min_max_kernel", &err);

  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  // std::cout << "Kernel created." << std::endl;
  cl_event events[2];

  auto local_work_size = get_local_work_size_small(device);

  cl_uint divider = 128;
  while (true) {
    if (inputSize[0] % divider == 0) {
      break;
    }
    divider /= 2;
    if (divider == 2) {
      break;
    }
  }

  const cl_uint4 outputDim = {{static_cast<cl_uint>(inputSize[0]) / divider,
                               static_cast<cl_uint>(inputSize[1]),
                               static_cast<cl_uint>(inputSize[2]), 0}};

  const size_t global_work_size = outputDim.x * outputDim.y * outputDim.z;

  const auto memorySizeSub =
      outputDim.x * outputDim.y * outputDim.z * sizeof(cl_float2);
  // std::cout << "MMM::output xy size: " << outputDim.x * outputDim.y *
  // outputDim.z << std::endl;
  auto *sub_buffer = new cl_float2[outputDim.x * outputDim.y * outputDim.z];
  auto deviceSubBuffer =
      clCreateBuffer(ctx, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeSub, (void *)&sub_buffer[0], &err);
  // err = clEnqueueWriteBuffer(queue, deviceSubBuffer, CL_TRUE, 0,
  // memorySizeSub, sub_buffer, 0, NULL, NULL);

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_mem_arg<1>(m_Kernel, deviceSubBuffer);
  set_kernel_arg<2>(m_Kernel, divider);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceSubBuffer, CL_TRUE, 0, memorySizeSub,
                            sub_buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceSubBuffer);
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "MMM::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  return OpenCL_min_max_recurse(sub_buffer,
                                static_cast<cl_uint>(global_work_size));
}

cl_float2 OpenCL_min_max_2D(const cl_float *buffer,
                            const itk::Image<float, 2U>::SizeType& inputSize) {

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput = inputSize[0] * inputSize[1] * sizeof(cl_float);
  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);
  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not create OpenCL mem object, error code: " << err
              << std::endl;
  }
  /*
  err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
  buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS)
          std::cout << "MM2D::Could not create OpenCL buffers, error code: " <<
  err << std::endl; //-38
          */
  // Create program
  // std::cout << "build min max kernel..." << std::endl;
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "min_max_kernel", &err);

  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  // std::cout << "Kernel created." << std::endl;
  cl_event events[2];

  auto local_work_size = get_local_work_size_small(device);

  cl_uint divider = 128;
  while (true) {
    if (inputSize[0] % divider == 0) {
      break;
    }
    divider /= 2;
    if (divider == 2) {
      break;
    }
  }

  const cl_uint2 outputDim = {{static_cast<cl_uint>(inputSize[0]) / divider,
                               static_cast<cl_uint>(inputSize[1])}};

  const size_t global_work_size = outputDim.x * outputDim.y;

  const auto memorySizeSub = outputDim.x * outputDim.y * sizeof(cl_float2);
  // std::cout << "MMM::output xy size: " << outputDim.x * outputDim.y *
  // outputDim.z << std::endl;
  auto *sub_buffer = new cl_float2[outputDim.x * outputDim.y];
  auto deviceSubBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                     memorySizeSub, (void *)&sub_buffer[0], &err);
  // err = clEnqueueWriteBuffer(queue, deviceSubBuffer, CL_TRUE, 0,
  // memorySizeSub, sub_buffer, 0, NULL, NULL);

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_mem_arg<1>(m_Kernel, deviceSubBuffer);
  set_kernel_arg<2>(m_Kernel, divider);

  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceSubBuffer, CL_TRUE, 0, memorySizeSub,
                            sub_buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceSubBuffer);
  clReleaseMemObject(deviceBuffer);

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "MM2D::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  return OpenCL_min_max_recurse(sub_buffer,
                                static_cast<cl_uint>(global_work_size));
}

cl_float2 OpenCL_min_max_recurse(const cl_float2 *buffer,
                                 const cl_uint inputSize) {

  if (inputSize < 257) {
    cl_float2 out = {{65535.0, -9999.0}};
    for (cl_uint i = 0; i < inputSize; i++) {
      if (out.x > buffer[i].x) {
        out.x = buffer[i].x;
      }
      if (out.y < buffer[i].y) {
        out.y = buffer[i].y;
      }
    }
    return out;
  }

  auto err = CL_SUCCESS;

  cl_program m_Program;

  const auto memorySizeInput = inputSize * sizeof(cl_float2);

  const auto ctx_queue_dev = get_constext_queue(memorySizeInput);
  const auto ctx = std::get<0>(ctx_queue_dev);
  const auto queue = std::get<1>(ctx_queue_dev);
  const auto device = std::get<2>(ctx_queue_dev);

  // std::cout << "MMR::Input xy size: " << inputSize << std::endl;

  /* Prepare OpenCL memory objects and place data inside them. */
  auto deviceBuffer =
      clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeInput, (void *)&buffer[0], &err);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not create OpenCL mem object, error code: " << err
              << std::endl;
  }
  /*
  err = clEnqueueWriteBuffer(queue, deviceBuffer, CL_TRUE, 0, memorySizeInput,
  buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS)
          std::cout << "MMR::Could not create OpenCL buffers of size: " <<
  inputSize << "*sizeof(cl_float2), error code: " << err << std::endl;
          */

  cl_uint divider = 128;
  while (true) {
    if (inputSize % divider == 0) {
      break;
    }
    divider /= 2;
    if (divider == 2) {
      break;
    }
  }

  const auto outputDim = inputSize / divider;
  auto local_work_size = get_local_work_size_large(device);

  while (local_work_size > 32) {
    if ((outputDim + outputDim % local_work_size) / local_work_size % 2 !=
        0) { // global must be evenly divisable with local work group
      local_work_size /= 2;
    } else {
      break;
    }
  }
  const auto global_work_size = (outputDim + outputDim % local_work_size) /
                                local_work_size *
                                local_work_size; // seems redundant, but isn't
  // std::cout << "global/local = " << (float)global_work_size /
  // (float)local_work_size << std::endl;
  const auto memorySizeSub =
      global_work_size *
      sizeof(cl_float2); // to avoid access violation in kernel
  // std::cout << "MMM::output xy size: " << outputDim << std::endl;
  auto *sub_buffer = new cl_float2[global_work_size];
  auto deviceSubBuffer =
      clCreateBuffer(ctx, CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                     memorySizeSub, (void *)&sub_buffer[0], &err);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not create OpenCL buffers of size: "
              << global_work_size << "*sizeof(cl_float2), error code: " << err
              << std::endl;
  }
  /*
  err = clEnqueueWriteBuffer(queue, deviceSubBuffer, CL_TRUE, 0, memorySizeSub,
  sub_buffer, 0, NULL, NULL);

  if (err != CL_SUCCESS)
          std::cout << "MMR::Could not write OpenCL buffers of size: " <<
  global_work_size << "*sizeof(cl_float2), error code: " << err << std::endl;
          */
  // Create program
  // std::cout << "build min max kernel 2..." << std::endl;
  std::string cl_file("fdk_opencl.cl");
  CreateAndBuildOpenCLProgramFromSourceFile(cl_file, ctx, m_Program);

  auto m_Kernel = clCreateKernel(m_Program, "min_max_kernel2", &err);

  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  // std::cout << "Kernel created." << std::endl;

  set_kernel_mem_arg<0>(m_Kernel, deviceBuffer);
  set_kernel_mem_arg<1>(m_Kernel, deviceSubBuffer);
  set_kernel_arg<2>(m_Kernel, divider);
  set_kernel_arg<3>(m_Kernel, outputDim);

  cl_event events[2];
  // Execute kernel
  err = clEnqueueNDRangeKernel(queue, m_Kernel, 1, nullptr, &global_work_size,
                               &local_work_size, 0, nullptr, &events[0]);

  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not run OpenCL kernel, error code: " << err
              << std::endl;
  }

  err = clWaitForEvents(1, &events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not wait for OpenCL events!?, error code: " << err
              << std::endl;
  }
  err = clReleaseEvent(events[0]);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not release OpenCL event, error code: " << err
              << std::endl;
  }

  /* Wait for calculations to be finished. */
  err = clReleaseKernel(m_Kernel);
  err |= clReleaseProgram(m_Program);
  err |= clFinish(queue);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not finish OpenCL queue, error code: " << err
              << std::endl;
  }

  /* Fetch results of calculations. */
  err = clEnqueueReadBuffer(queue, deviceSubBuffer, CL_TRUE, 0, memorySizeSub,
                            sub_buffer, 0, nullptr, nullptr);
  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not read OpenCL buffer, error code: " << err
              << std::endl;
  }

  /* Release OpenCL working objects. */
  clReleaseMemObject(deviceSubBuffer);
  clReleaseMemObject(deviceBuffer);
  // delete[] buffer; // Not our responsibility!

  release_ctx_queue(ctx, queue);

  if (err != CL_SUCCESS) {
    std::cout << "MMR::Could not create OpenCL kernel, error code: " << err
              << std::endl;
  }

  return OpenCL_min_max_recurse(sub_buffer, outputDim);
}
