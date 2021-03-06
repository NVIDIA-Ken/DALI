// Copyright (c) 2017-2018, NVIDIA CORPORATION. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dali/pipeline/executor/executor.h"

#include <opencv2/opencv.hpp>

#include "dali/pipeline/operators/util/external_source.h"
#include "dali/test/dali_test.h"

namespace dali {

namespace {
// Our turbo jpeg decoder cannot handle CMYK images
// or 410 images
const vector<string> tjpg_test_images = {
  image_folder + "/420.jpg",
  image_folder + "/422.jpg",
  image_folder + "/440.jpg",
  image_folder + "/444.jpg",
  image_folder + "/gray.jpg",
  image_folder + "/411.jpg",
  image_folder + "/411-non-multiple-4-width.jpg",
  image_folder + "/420-odd-height.jpg",
  image_folder + "/420-odd-width.jpg",
  image_folder + "/420-odd-both.jpg",
  image_folder + "/422-odd-width.jpg"
};
}  // namespace

class ExecutorTest : public DALITest {
 public:
  void SetUp() override {
    rand_gen_.seed(time(nullptr));
    LoadJPEGS(tjpg_test_images, &jpegs_, &jpeg_sizes_);
    batch_size_ = jpegs_.size();
    DecodeJPEGS(DALI_RGB);
  }

  inline void set_batch_size(int size) { batch_size_ = size; }

  inline OpSpec PrepareSpec(OpSpec spec) {
    spec.AddArg("batch_size", batch_size_)
      .AddArg("num_threads", num_threads_);
    return spec;
  }

  inline void PruneGraph(Executor *exe) {
    exe->PruneUnusedGraphNodes();
  }

  vector<HostWorkspace> CPUData(Executor *exe, int idx) {
    return exe->wss_[idx].cpu_op_data;
  }

  vector<MixedWorkspace> MixedData(Executor *exe, int idx) {
    return exe->wss_[idx].mixed_op_data;
  }

  vector<DeviceWorkspace> GPUData(Executor *exe, int idx) {
    return exe->wss_[idx].gpu_op_data;
  }

  void VerifyDecode(const uint8 *img, int h, int w, int img_id) {
    // Load the image to host
    uint8 *host_img = new uint8[h*w*c_];
    CUDA_CALL(cudaMemcpy(host_img, img, h*w*c_, cudaMemcpyDefault));

    // Compare w/ opencv result
    cv::Mat ver;
    cv::Mat jpeg = cv::Mat(1, jpeg_sizes_[img_id], CV_8UC1, jpegs_[img_id]);

    ASSERT_TRUE(CheckIsJPEG(jpegs_[img_id], jpeg_sizes_[img_id]));
    int flag = IsColor(img_type_) ? CV_LOAD_IMAGE_COLOR : CV_LOAD_IMAGE_GRAYSCALE;
    cv::imdecode(jpeg, flag, &ver);

    cv::Mat ver_img(h, w, IsColor(img_type_) ? CV_8UC3 : CV_8UC2);
    if (img_type_ == DALI_RGB) {
      // Convert from BGR to RGB for verification
      cv::cvtColor(ver, ver_img, CV_BGR2RGB);
    } else {
      ver_img = ver;
    }

    // DEBUG
    // WriteHWCImage(ver_img.ptr(), h, w, c_, std::to_string(img_id) + "-ver");

    ASSERT_EQ(h, ver_img.rows);
    ASSERT_EQ(w, ver_img.cols);
    vector<int> diff(h*w*c_, 0);
    for (int i = 0; i < h*w*c_; ++i) {
      diff[i] = abs(static_cast<int>(ver_img.ptr()[i] - host_img[i]));
    }

    // calculate the MSE
    double mean, std;
    this->MeanStdDev(diff, &mean, &std);

#ifndef NDEBUG
    cout << "num: " << diff.size() << endl;
    cout << "mean: " << mean << endl;
    cout << "std: " << std << endl;
#endif

    // Note: We allow a slight deviation from the ground truth.
    // This value was picked fairly arbitrarily to let the test
    // pass for libjpeg turbo
    ASSERT_LT(mean, 2.f);
    ASSERT_LT(std, 3.f);
  }

 protected:
  int batch_size_, num_threads_ = 1;
  int c_ = 3;
  DALIImageType img_type_ = DALI_RGB;
};

TEST_F(ExecutorTest, TestPruneBasicGraph) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddOutput("data1", "cpu")
          .AddOutput("data2", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data3", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("data3", "cpu")
          .AddOutput("data3_cont", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data4", "cpu")), "");

  vector<string> outputs = {"data3_cont_cpu"};
  exe.Build(&graph, outputs);

  // Validate the graph - op 3 should
  // have been pruned as its outputs
  // are unused.
  ASSERT_EQ(graph.NumCPUOp(), 2);
  ASSERT_EQ(graph.NumMixedOp(), 1);
  ASSERT_EQ(graph.NumGPUOp(), 0);

  // Validate the source op
  auto& node = graph.node(0);
  ASSERT_EQ(node.id, 0);
  ASSERT_EQ(node.children.size(), 1);
  ASSERT_EQ(node.parents.size(), 0);
  ASSERT_EQ(node.children.count(1), 1);
  ASSERT_EQ(graph.TensorSourceID(node.spec.Output(0)), 0);
  ASSERT_EQ(graph.TensorIdxInSource(node.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node.spec.Output(0)));

  auto& node2 = graph.node(1);
  ASSERT_EQ(node2.id, 1);
  ASSERT_EQ(node2.children.size(), 1);
  ASSERT_EQ(node2.parents.size(), 1);
  ASSERT_EQ(node2.parents.count(0), 1);
  ASSERT_EQ(graph.TensorSourceID(node2.spec.Output(0)), 1);
  ASSERT_EQ(graph.TensorIdxInSource(node2.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node2.spec.Output(0)));
  ASSERT_EQ(node2.spec.Output(0), "data3_cpu");

  auto& node3 = graph.node(2);
  ASSERT_EQ(node3.id, 2);
  ASSERT_EQ(node3.children.size(), 0);
  ASSERT_EQ(node3.parents.size(), 1);
  ASSERT_EQ(node3.parents.count(1), 1);
  ASSERT_EQ(graph.TensorSourceID(node3.spec.Output(0)), 2);
  ASSERT_EQ(graph.TensorIdxInSource(node3.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node3.spec.Output(0)));
  ASSERT_EQ(node3.spec.Output(0), "data3_cont_cpu");
}

TEST_F(ExecutorTest, TestPruneMultiple) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddOutput("data1", "cpu")
          .AddOutput("data2", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("data1", "cpu")
          .AddOutput("data1_cont", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data3", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data4", "cpu")), "");

  vector<string> outputs = {"data1_cont_cpu"};
  exe.Build(&graph, outputs);

  // Validate the graph - op 2&3 should
  // have been pruned
  ASSERT_EQ(graph.NumCPUOp(), 1);
  ASSERT_EQ(graph.NumMixedOp(), 1);
  ASSERT_EQ(graph.NumGPUOp(), 0);

  // Validate the source op
  auto& node = graph.node(0);
  ASSERT_EQ(node.id, 0);
  ASSERT_EQ(node.children.size(), 1);
  ASSERT_EQ(node.parents.size(), 0);
  ASSERT_EQ(graph.TensorSourceID(node.spec.Output(0)), 0);
  ASSERT_EQ(graph.TensorIdxInSource(node.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node.spec.Output(0)));
  ASSERT_EQ(node.spec.NumOutput(), 2);
  ASSERT_EQ(node.spec.Output(0), "data1_cpu");
  ASSERT_EQ(node.spec.Output(1), "data2_cpu");

  auto& node2 = graph.node(1);
  ASSERT_EQ(node2.id, 1);
  ASSERT_EQ(node2.children.size(), 0);
  ASSERT_EQ(node2.parents.size(), 1);
  ASSERT_EQ(graph.TensorSourceID(node2.spec.Output(0)), 1);
  ASSERT_EQ(graph.TensorIdxInSource(node2.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node2.spec.Output(0)));
  ASSERT_EQ(node2.spec.NumOutput(), 1);
  ASSERT_EQ(node2.spec.Output(0), "data1_cont_cpu");
}

TEST_F(ExecutorTest, TestPruneRecursive) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddOutput("data1", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("data1", "cpu")
          .AddOutput("data1_cont", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data2", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data2", "cpu")
          .AddOutput("data3", "cpu")), "");

  vector<string> outputs = {"data1_cont_cpu"};
  exe.Build(&graph, outputs);

  // Validate the graph - op 2&3 should
  // have been pruned
  ASSERT_EQ(graph.NumCPUOp(), 1);
  ASSERT_EQ(graph.NumMixedOp(), 1);
  ASSERT_EQ(graph.NumGPUOp(), 0);

  // Validate the source op
  auto& node = graph.node(0);
  ASSERT_EQ(node.id, 0);
  ASSERT_EQ(node.children.size(), 1);
  ASSERT_EQ(node.parents.size(), 0);
  ASSERT_EQ(graph.TensorSourceID(node.spec.Output(0)), 0);
  ASSERT_EQ(graph.TensorIdxInSource(node.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node.spec.Output(0)));
  ASSERT_EQ(node.spec.NumOutput(), 1);
  ASSERT_EQ(node.spec.Output(0), "data1_cpu");

  auto& node2 = graph.node(1);
  ASSERT_EQ(node2.id, 1);
  ASSERT_EQ(node2.children.size(), 0);
  ASSERT_EQ(node2.parents.size(), 1);
  ASSERT_EQ(graph.TensorSourceID(node2.spec.Output(0)), 1);
  ASSERT_EQ(graph.TensorIdxInSource(node2.spec.Output(0)), 0);
  ASSERT_TRUE(graph.TensorIsType<CPUBackend>(node2.spec.Output(0)));
  ASSERT_EQ(node2.spec.NumOutput(), 1);
  ASSERT_EQ(node2.spec.Output(0), "data1_cont_cpu");
}

TEST_F(ExecutorTest, TestPruneWholeGraph) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddOutput("data1", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data1", "cpu")
          .AddOutput("data2", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "cpu")
          .AddArg("num_outputs", 1)
          .AddInput("data2", "cpu")
          .AddOutput("data3", "cpu")), "");

  vector<string> outputs = {"data_that_does_not_exist"};
  ASSERT_THROW(this->PruneGraph(&exe),
      std::runtime_error);
}

TEST_F(ExecutorTest, TestDataSetup) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("ExternalSource")
          .AddArg("device", "cpu")
          .AddOutput("data1", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("data1", "cpu")
          .AddOutput("data2", "gpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("DummyOp")
          .AddArg("device", "gpu")
          .AddArg("num_outputs", 1)
          .AddInput("data2", "gpu")
          .AddOutput("data3", "gpu")), "");

  vector<string> outputs = {"data3_gpu"};
  exe.Build(&graph, outputs);

  // Verify the data has been setup correctly
  for (int i = 0; i < 2; ++i) {
    auto host_workspaces = this->CPUData(&exe, i);
    ASSERT_EQ(host_workspaces.size(), 1);
    HostWorkspace &hws = host_workspaces[0];
    ASSERT_EQ(hws.NumInput(), 0);
    ASSERT_EQ(hws.NumOutput(), 1);
    ASSERT_EQ(hws.NumOutputAtIdx(0), batch_size_);
    ASSERT_TRUE(hws.OutputIsType<CPUBackend>(0));

    auto mixed_workspaces = this->MixedData(&exe, i);
    ASSERT_EQ(mixed_workspaces.size(), 1);
    MixedWorkspace &mws = mixed_workspaces[0];
    ASSERT_EQ(mws.NumInput(), 1);
    ASSERT_EQ(mws.NumInputAtIdx(0), batch_size_);
    ASSERT_TRUE(mws.InputIsType<CPUBackend>(0));
    ASSERT_EQ(mws.NumOutput(), 1);
    ASSERT_TRUE(mws.OutputIsType<GPUBackend>(0));

    auto device_workspaces = this->GPUData(&exe, i);
    ASSERT_EQ(device_workspaces.size(), 1);
    DeviceWorkspace &dws = device_workspaces[0];
    ASSERT_EQ(dws.NumInput(), 1);
    ASSERT_TRUE(dws.InputIsType<GPUBackend>(0));
    ASSERT_EQ(dws.NumOutput(), 1);
    ASSERT_TRUE(dws.OutputIsType<GPUBackend>(0));
  }
}

TEST_F(ExecutorTest, TestRunBasicGraph) {
  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("ExternalSource")
          .AddArg("device", "cpu")
          .AddOutput("data", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("HostDecoder")
          .AddArg("device", "cpu")
          .AddInput("data", "cpu")
          .AddOutput("images", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("images", "cpu")
          .AddOutput("final_images", "cpu")), "");

  vector<string> outputs = {"final_images_cpu"};
  exe.Build(&graph, outputs);

  // Set the data for the external source
  auto *src_op = dynamic_cast<ExternalSource<CPUBackend>*>(&graph.cpu_op(0));
  ASSERT_NE(src_op, nullptr);
  TensorList<CPUBackend> tl;
  this->MakeJPEGBatch(&tl, this->batch_size_);
  src_op->SetDataSource(tl);

  exe.RunCPU();
  exe.RunMixed();
  exe.RunGPU();

  DeviceWorkspace ws;
  exe.Outputs(&ws);
  ASSERT_EQ(ws.NumOutput(), 1);
  ASSERT_EQ(ws.NumInput(), 0);
  ASSERT_TRUE(ws.OutputIsType<CPUBackend>(0));
}

TEST_F(ExecutorTest, TestPrefetchedExecution) {
  int batch_size = this->batch_size_ / 2;
  this->set_batch_size(batch_size);

  Executor exe(this->batch_size_, this->num_threads_, 0, 1);

  // Build a basic cpu->gpu graph
  OpGraph graph;
  graph.AddOp(this->PrepareSpec(
          OpSpec("ExternalSource")
          .AddArg("device", "cpu")
          .AddOutput("data", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("HostDecoder")
          .AddArg("device", "cpu")
          .AddInput("data", "cpu")
          .AddOutput("images", "cpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("MakeContiguous")
          .AddArg("device", "mixed")
          .AddInput("images", "cpu")
          .AddOutput("images", "gpu")), "");

  graph.AddOp(this->PrepareSpec(
          OpSpec("Copy")
          .AddArg("device", "gpu")
          .AddInput("images", "gpu")
          .AddOutput("final_images", "gpu")), "");

  vector<string> outputs = {"final_images_gpu"};
  exe.Build(&graph, outputs);

  // Set the data for the external source
  auto *src_op = dynamic_cast<ExternalSource<CPUBackend>*>(&graph.cpu_op(0));
  ASSERT_NE(src_op, nullptr);
  TensorList<CPUBackend> tl;
  this->MakeJPEGBatch(&tl, this->batch_size_*2);

  // Split the batch into two
  TensorList<CPUBackend> tl2;
  TensorList<CPUBackend> tl1;
  vector<Dims> shape1(batch_size), shape2(batch_size);
  for (int i = 0; i < batch_size; ++i) {
    shape1[i] = tl.tensor_shape(i);
    shape2[i] = tl.tensor_shape(i+batch_size);
  }
  tl1.Resize(shape1);
  tl2.Resize(shape2);
  for (int i = 0; i < batch_size; ++i) {
    std::memcpy(
        tl1.template mutable_tensor<uint8>(i),
        tl.template tensor<uint8>(i),
        Product(tl.tensor_shape(i)));
    std::memcpy(
        tl2.template mutable_tensor<uint8>(i),
        tl.template tensor<uint8>(i+batch_size),
        Product(tl.tensor_shape(i+batch_size)));
  }

  // Run twice without getting the results
  src_op->SetDataSource(tl1);
  exe.RunCPU();
  exe.RunMixed();
  exe.RunGPU();

  src_op->SetDataSource(tl2);
  exe.RunCPU();
  exe.RunMixed();
  exe.RunGPU();

  // Verify that both sets of results are correct
  DeviceWorkspace ws;
  exe.Outputs(&ws);
  ASSERT_EQ(ws.NumOutput(), 1);
  ASSERT_EQ(ws.NumInput(), 0);
  ASSERT_TRUE(ws.OutputIsType<GPUBackend>(0));
  TensorList<GPUBackend> *res1 = ws.Output<GPUBackend>(0);
  for (int i = 0; i < batch_size; ++i) {
    this->VerifyDecode(
        res1->template tensor<uint8>(i),
        res1->tensor_shape(i)[0],
        res1->tensor_shape(i)[1], i);
  }

  exe.Outputs(&ws);
  ASSERT_EQ(ws.NumOutput(), 1);
  ASSERT_EQ(ws.NumInput(), 0);
  ASSERT_TRUE(ws.OutputIsType<GPUBackend>(0));
  TensorList<GPUBackend> *res2 = ws.Output<GPUBackend>(0);
  for (int i = 0; i < batch_size; ++i) {
    this->VerifyDecode(
        res2->template tensor<uint8>(i),
        res2->tensor_shape(i)[0],
        res2->tensor_shape(i)[1],
        i+batch_size);
  }
}

}  // namespace dali
