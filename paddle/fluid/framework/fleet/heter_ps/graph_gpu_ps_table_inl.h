// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
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

#pragma once
#ifdef PADDLE_WITH_HETERPS
//#include "paddle/fluid/framework/fleet/heter_ps/graph_gpu_ps_table.h"
namespace paddle {
namespace framework {
/*
comment 0
this kernel just serves as an example of how to sample nodes' neighbors.
feel free to modify it
index[0,len) saves the nodes' index
actual_size[0,len) is to save the sample size of each node.
for ith node in index, actual_size[i] = min(node i's neighbor size, sample size)
sample_result is to save the neighbor sampling result, its size is len *
sample_size;

*/
__global__ void neighbor_sample_example(GpuPsCommGraph graph, int* node_index,
                                        int* actual_size, int64_t* res,
                                        int sample_len, int* sample_status,
                                        int n, int from) {
  int id = blockIdx.x * blockDim.y + threadIdx.y;
  if (id < n) {
    if (node_index[id] == -1) {
      actual_size[id] = 0;
      return;
    }
    curandState rng;
    curand_init(blockIdx.x, threadIdx.x, threadIdx.y, &rng);
    int index = threadIdx.x;
    int offset = id * sample_len;
    int64_t* data = graph.neighbor_list;
    int data_offset = graph.node_list[node_index[id]].neighbor_offset;
    int neighbor_len = graph.node_list[node_index[id]].neighbor_size;
    int ac_len;
    if (sample_len > neighbor_len)
      ac_len = neighbor_len;
    else {
      ac_len = sample_len;
    }
    if (4 * ac_len >= 3 * neighbor_len) {
      if (index == 0) {
        res[offset] = curand(&rng) % (neighbor_len - ac_len + 1);
      }
      __syncwarp();
      int start = res[offset];
      while (index < ac_len) {
        res[offset + index] = data[data_offset + start + index];
        index += blockDim.x;
      }
      actual_size[id] = ac_len;
    } else {
      while (index < ac_len) {
        int num = curand(&rng) % neighbor_len;
        int* addr = sample_status + data_offset + num;
        int expected = *addr;
        if (!(expected & (1 << from))) {
          int old = atomicCAS(addr, expected, expected | (1 << from));
          if (old == expected) {
            res[offset + index] = num;
            index += blockDim.x;
          }
        }
      }
      __syncwarp();
      index = threadIdx.x;
      while (index < ac_len) {
        int* addr = sample_status + data_offset + res[offset + index];
        int expected, old = *addr;
        do {
          expected = old;
          old = atomicCAS(addr, expected, expected & (~(1 << from)));
        } while (old != expected);
        res[offset + index] = data[data_offset + res[offset + index]];
        index += blockDim.x;
      }
      actual_size[id] = ac_len;
    }
  }
  // const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  // if (i < n) {
  //   auto node_index = index[i];
  //   actual_size[i] = graph.node_list[node_index].neighbor_size < sample_size
  //                        ? graph.node_list[node_index].neighbor_size
  //                        : sample_size;
  //   int offset = graph.node_list[node_index].neighbor_offset;
  //   for (int j = 0; j < actual_size[i]; j++) {
  //     sample_result[sample_size * i + j] = graph.neighbor_list[offset + j];
  //   }
  // }
}

int GpuPsGraphTable::init_cpu_table(
    const paddle::distributed::GraphParameter& graph) {
  cpu_graph_table.reset(new paddle::distributed::GraphTable);
  cpu_table_status = cpu_graph_table->Initialize(graph);
  // if (cpu_table_status != 0) return cpu_table_status;
  // std::function<void(std::vector<GpuPsCommGraph>&)> callback =
  //     [this](std::vector<GpuPsCommGraph>& res) {
  //       pthread_rwlock_wrlock(this->rw_lock.get());
  //       this->clear_graph_info();
  //       this->build_graph_from_cpu(res);
  //       pthread_rwlock_unlock(this->rw_lock.get());
  //       cv_.notify_one();
  //     };
  // cpu_graph_table->set_graph_sample_callback(callback);
  return cpu_table_status;
}

// int GpuPsGraphTable::load(const std::string& path, const std::string& param)
// {
//   int status = cpu_graph_table->load(path, param);
//   if (status != 0) {
//     return status;
//   }
//   std::unique_lock<std::mutex> lock(mutex_);
//   cpu_graph_table->start_graph_sampling();
//   cv_.wait(lock);
//   return 0;
// }
/*
 comment 1

 gpu i triggers a neighbor_sample task,
 when this task is done,
 this function is called to move the sample result on other gpu back
 to gup i and aggragate the result.
 the sample_result is saved on src_sample_res and the actual sample size for
 each node is saved on actual_sample_size.
 the number of actual sample_result for
 key[x] (refer to comment 2 for definition of key)
 is saved on  actual_sample_size[x], since the neighbor size of key[x] might be
 smaller than sample_size,
 is saved on src_sample_res [x*sample_size, x*sample_size +
 actual_sample_size[x])

 since before each gpu runs the neighbor_sample task,the key array is shuffled,
 but we have the idx array to save the original order.
 when the gpu i gets all the sample results from other gpus, it relies on
 idx array to recover the original order.
 that's what fill_dvals does.

*/

void GpuPsGraphTable::move_neighbor_sample_result_to_source_gpu(
    int start_index, int gpu_num, int sample_size, int* h_left, int* h_right,
    int64_t* src_sample_res, int* actual_sample_size) {
  int shard_len[gpu_num];
  for (int i = 0; i < gpu_num; i++) {
    if (h_left[i] == -1 || h_right[i] == -1) {
      continue;
    }
    shard_len[i] = h_right[i] - h_left[i] + 1;
    int cur_step = path_[start_index][i].nodes_.size() - 1;
    for (int j = cur_step; j > 0; j--) {
      cudaMemcpyAsync(path_[start_index][i].nodes_[j - 1].val_storage,
                      path_[start_index][i].nodes_[j].val_storage,
                      path_[start_index][i].nodes_[j - 1].val_bytes_len,
                      cudaMemcpyDefault,
                      path_[start_index][i].nodes_[j - 1].out_stream);
    }
    auto& node = path_[start_index][i].nodes_.front();
    cudaMemcpyAsync(
        reinterpret_cast<char*>(src_sample_res + h_left[i] * sample_size),
        node.val_storage + sizeof(int64_t) * shard_len[i],
        node.val_bytes_len - sizeof(int64_t) * shard_len[i], cudaMemcpyDefault,
        node.out_stream);
    // resource_->remote_stream(i, start_index));
    cudaMemcpyAsync(reinterpret_cast<char*>(actual_sample_size + h_left[i]),
                    node.val_storage + sizeof(int) * shard_len[i],
                    sizeof(int) * shard_len[i], cudaMemcpyDefault,
                    node.out_stream);
  }
  for (int i = 0; i < gpu_num; ++i) {
    if (h_left[i] == -1 || h_right[i] == -1) {
      continue;
    }
    auto& node = path_[start_index][i].nodes_.front();
    cudaStreamSynchronize(node.out_stream);
    // cudaStreamSynchronize(resource_->remote_stream(i, start_index));
  }
  /*
    std::queue<CopyTask> que;
    // auto& node = path_[gpu_id][i].nodes_.front();
    // cudaMemcpyAsync(
    //     reinterpret_cast<char*>(src_sample_res + h_left[i] * sample_size),
    //     node.val_storage + sizeof(int64_t) * shard_len,
    //     node.val_bytes_len - sizeof(int64_t) * shard_len, cudaMemcpyDefault,
    //     node.out_stream);
    // cudaMemcpyAsync(reinterpret_cast<char*>(actual_sample_size + h_left[i]),
    //                 node.val_storage + sizeof(int) * shard_len,
    //                 sizeof(int) * shard_len, cudaMemcpyDefault,
    //                 node.out_stream);
    int cur_step = path_[start_index][i].nodes_.size() - 1;
    auto& node = path_[start_index][i].nodes_[cur_step];
    if (cur_step == 0) {
      // cudaMemcpyAsync(reinterpret_cast<char*>(src_val + h_left[i]),
      //                 node.val_storage, node.val_bytes_len,
      //                 cudaMemcpyDefault,
      //                 node.out_stream);
     // VLOG(0)<<"copy "<<node.gpu_num<<" to "<<start_index;
      cudaMemcpyAsync(
          reinterpret_cast<char*>(src_sample_res + h_left[i] * sample_size),
          node.val_storage + sizeof(int64_t) * shard_len[i],
          node.val_bytes_len - sizeof(int64_t) * shard_len[i],
          cudaMemcpyDefault,
          node.out_stream);
          //resource_->remote_stream(i, start_index));
      cudaMemcpyAsync(reinterpret_cast<char*>(actual_sample_size + h_left[i]),
                      node.val_storage + sizeof(int) * shard_len[i],
                      sizeof(int) * shard_len[i], cudaMemcpyDefault,
                      node.out_stream);
                      //resource_->remote_stream(i, start_index));
    } else {
      CopyTask t(&path_[start_index][i], cur_step - 1);
      que.push(t);
       //     VLOG(0)<<"copy "<<node.gpu_num<<" to
  "<<path_[start_index][i].nodes_[cur_step - 1].gpu_num;
      cudaMemcpyAsync(path_[start_index][i].nodes_[cur_step - 1].val_storage,
                      node.val_storage,
                      path_[start_index][i].nodes_[cur_step - 1].val_bytes_len,
                      cudaMemcpyDefault,
                     path_[start_index][i].nodes_[cur_step - 1].out_stream);
                     //resource_->remote_stream(i, start_index));
    }
  }
  while (!que.empty()) {
    CopyTask& cur_task = que.front();
    que.pop();
    int cur_step = cur_task.step;
    if (cur_task.path->nodes_[cur_step].sync) {
      cudaStreamSynchronize(cur_task.path->nodes_[cur_step].out_stream);
      //cudaStreamSynchronize(resource_->remote_stream(cur_task.path->nodes_.back().gpu_num,
  start_index));
    }
    if (cur_step > 0) {
      CopyTask c(cur_task.path, cur_step - 1);
      que.push(c);
      cudaMemcpyAsync(cur_task.path->nodes_[cur_step - 1].val_storage,
                      cur_task.path->nodes_[cur_step].val_storage,
                      cur_task.path->nodes_[cur_step - 1].val_bytes_len,
                      cudaMemcpyDefault,
                      cur_task.path->nodes_[cur_step - 1].out_stream);
                      //resource_->remote_stream(cur_task.path->nodes_.back().gpu_num,
  start_index));
    } else if (cur_step == 0) {
      int end_index = cur_task.path->nodes_.back().gpu_num;
      // cudaMemcpyAsync(reinterpret_cast<char*>(src_val + h_left[end_index]),
      //                 cur_task.path->nodes_[cur_step].val_storage,
      //                 cur_task.path->nodes_[cur_step].val_bytes_len,
      //                 cudaMemcpyDefault,
      //                 cur_task.path->nodes_[cur_step].out_stream);
      //VLOG(0)<<"copy "<<cur_task.path->nodes_[cur_step].gpu_num<< " to
  "<<start_index;
      cudaMemcpyAsync(reinterpret_cast<char*>(src_sample_res +
                                              h_left[end_index] * sample_size),
                      cur_task.path->nodes_[cur_step].val_storage +
                          sizeof(int64_t) * shard_len[end_index],
                      cur_task.path->nodes_[cur_step].val_bytes_len -
                          sizeof(int64_t) * shard_len[end_index],
                      cudaMemcpyDefault,
                      cur_task.path->nodes_[cur_step].out_stream);
                      //resource_->remote_stream(cur_task.path->nodes_.back().gpu_num,
  start_index));
      cudaMemcpyAsync(
          reinterpret_cast<char*>(actual_sample_size + h_left[end_index]),
          cur_task.path->nodes_[cur_step].val_storage +
              sizeof(int) * shard_len[end_index],
          sizeof(int) * shard_len[end_index], cudaMemcpyDefault,
          cur_task.path->nodes_[cur_step].out_stream);
          //resource_->remote_stream(cur_task.path->nodes_.back().gpu_num,
  start_index));
    }
  }
  for (int i = 0; i < gpu_num; ++i) {
    if (h_left[i] == -1 || h_right[i] == -1) {
      continue;
    }
    auto& node = path_[start_index][i].nodes_.front();
    cudaStreamSynchronize(node.out_stream);
    //cudaStreamSynchronize(resource_->remote_stream(i, start_index));
  }
  */
}

/*
TODO:
how to optimize it to eliminate the for loop
*/
__global__ void fill_dvalues(int64_t* d_shard_vals, int64_t* d_vals,
                             int* d_shard_actual_sample_size,
                             int* d_actual_sample_size, int* idx,
                             int sample_size, int len) {
  const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < len) {
    d_actual_sample_size[idx[i]] = d_shard_actual_sample_size[i];
    for (int j = 0; j < sample_size; j++) {
      d_vals[idx[i] * sample_size + j] = d_shard_vals[i * sample_size + j];
    }
  }
}

__global__ void node_query_example(GpuPsCommGraph graph, int start, int size,
                                   int64_t* res) {
  const size_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < size) {
    res[i] = graph.node_list[start + i].node_id;
  }
}

void GpuPsGraphTable::clear_graph_info() {
  if (tables_.size()) {
    for (auto table : tables_) delete table;
  }
  tables_.clear();
  for (auto graph : gpu_graph_list) {
    if (graph.neighbor_list != NULL) {
      cudaFree(graph.neighbor_list);
    }
    if (graph.node_list != NULL) {
      cudaFree(graph.node_list);
    }
  }
  gpu_graph_list.clear();
}
/*
the parameter std::vector<GpuPsCommGraph> cpu_graph_list is generated by cpu.
it saves the graph to be saved on each gpu.

for the ith GpuPsCommGraph, any the node's key satisfies that key % gpu_number
== i

In this function, memory is allocated on each gpu to save the graphs,
gpu i saves the ith graph from cpu_graph_list
*/

void GpuPsGraphTable::build_graph_from_cpu(
    std::vector<GpuPsCommGraph>& cpu_graph_list) {
  VLOG(0) << "in build_graph_from_cpu cpu_graph_list size = "
          << cpu_graph_list.size();
  PADDLE_ENFORCE_EQ(
      cpu_graph_list.size(), resource_->total_device(),
      platform::errors::InvalidArgument("the cpu node list size doesn't match "
                                        "the number of gpu on your machine."));
  clear_graph_info();
  for (int i = 0; i < cpu_graph_list.size(); i++) {
    platform::CUDADeviceGuard guard(resource_->dev_id(i));
    // platform::CUDADeviceGuard guard(i);
    gpu_graph_list.push_back(GpuPsCommGraph());
    sample_status.push_back(NULL);
    auto table =
        new Table(std::max(1, cpu_graph_list[i].node_size) / load_factor_);
    tables_.push_back(table);
    if (cpu_graph_list[i].node_size > 0) {
      std::vector<int64_t> keys;
      std::vector<int> offset;
      cudaMalloc((void**)&gpu_graph_list[i].node_list,
                 cpu_graph_list[i].node_size * sizeof(GpuPsGraphNode));
      cudaMemcpy(gpu_graph_list[i].node_list, cpu_graph_list[i].node_list,
                 cpu_graph_list[i].node_size * sizeof(GpuPsGraphNode),
                 cudaMemcpyHostToDevice);
      for (int j = 0; j < cpu_graph_list[i].node_size; j++) {
        keys.push_back(cpu_graph_list[i].node_list[j].node_id);
        offset.push_back(j);
      }
      build_ps(i, keys.data(), offset.data(), keys.size(), 1024, 8);
      gpu_graph_list[i].node_size = cpu_graph_list[i].node_size;
    } else {
      build_ps(i, NULL, NULL, 0, 1024, 8);
      gpu_graph_list[i].node_list = NULL;
      gpu_graph_list[i].node_size = 0;
    }
    if (cpu_graph_list[i].neighbor_size) {
      int* addr;
      cudaMalloc((void**)&addr, cpu_graph_list[i].neighbor_size * sizeof(int));
      cudaMemset(addr, 0, cpu_graph_list[i].neighbor_size * sizeof(int));
      sample_status[i] = addr;
      cudaMalloc((void**)&gpu_graph_list[i].neighbor_list,
                 cpu_graph_list[i].neighbor_size * sizeof(int64_t));
      cudaMemcpy(gpu_graph_list[i].neighbor_list,
                 cpu_graph_list[i].neighbor_list,
                 cpu_graph_list[i].neighbor_size * sizeof(int64_t),
                 cudaMemcpyHostToDevice);
      gpu_graph_list[i].neighbor_size = cpu_graph_list[i].neighbor_size;
    } else {
      gpu_graph_list[i].neighbor_list = NULL;
      gpu_graph_list[i].neighbor_size = 0;
    }
  }
  cudaDeviceSynchronize();
}
NeighborSampleResult* GpuPsGraphTable::graph_neighbor_sample(int gpu_id,
                                                             int64_t* key,
                                                             int sample_size,
                                                             int len) {
  /*
 comment 2
  this function shares some kernels with heter_comm_inl.h
  arguments definitions:
  gpu_id:the id of gpu.
  len:how many keys are used,(the length of array key)
  sample_size:how many neighbors should be sampled for each node in key.

  the code below shuffle the key array to make the keys
    that belong to a gpu-card stay together,
    the shuffled result is saved on d_shard_keys,
    if ith element in d_shard_keys_ptr is
    from jth element in the original key array, then idx[i] = j,
    idx could be used to recover the original array.
    if keys in range [a,b] belong to ith-gpu, then h_left[i] = a, h_right[i] =
 b,
    if no keys are allocated for ith-gpu, then h_left[i] == h_right[i] == -1

    for example, suppose key = [0,1,2,3,4,5,6,7,8], gpu_num = 2
    when we run this neighbor_sample function,
    the key is shuffled to [0,2,4,6,8,1,3,5,7]
    the first part (0,2,4,6,8) % 2 == 0,thus should be handled by gpu 0,
    the rest part should be handled by gpu1, because (1,3,5,7) % 2 == 1,
    h_left = [0,5],h_right = [4,8]

  */

  NeighborSampleResult* result =
      new NeighborSampleResult(sample_size, len, resource_->dev_id(gpu_id));
  if (len == 0) {
    return result;
  }
  platform::CUDAPlace place = platform::CUDAPlace(resource_->dev_id(gpu_id));
  platform::CUDADeviceGuard guard(resource_->dev_id(gpu_id));
  // cudaMalloc((void**)&result->val, len * sample_size * sizeof(int64_t));
  // cudaMalloc((void**)&result->actual_sample_size, len * sizeof(int));
  int* actual_sample_size = result->actual_sample_size;
  int64_t* val = result->val;
  int total_gpu = resource_->total_device();
  // int dev_id = resource_->dev_id(gpu_id);
  auto stream = resource_->local_stream(gpu_id, 0);

  int grid_size = (len - 1) / block_size_ + 1;

  int h_left[total_gpu];   // NOLINT
  int h_right[total_gpu];  // NOLINT

  auto d_left = memory::Alloc(place, total_gpu * sizeof(int));
  auto d_right = memory::Alloc(place, total_gpu * sizeof(int));
  int* d_left_ptr = reinterpret_cast<int*>(d_left->ptr());
  int* d_right_ptr = reinterpret_cast<int*>(d_right->ptr());

  cudaMemsetAsync(d_left_ptr, -1, total_gpu * sizeof(int), stream);
  cudaMemsetAsync(d_right_ptr, -1, total_gpu * sizeof(int), stream);
  //
  auto d_idx = memory::Alloc(place, len * sizeof(int));
  int* d_idx_ptr = reinterpret_cast<int*>(d_idx->ptr());

  auto d_shard_keys = memory::Alloc(place, len * sizeof(int64_t));
  int64_t* d_shard_keys_ptr = reinterpret_cast<int64_t*>(d_shard_keys->ptr());
  auto d_shard_vals = memory::Alloc(place, sample_size * len * sizeof(int64_t));
  int64_t* d_shard_vals_ptr = reinterpret_cast<int64_t*>(d_shard_vals->ptr());
  auto d_shard_actual_sample_size = memory::Alloc(place, len * sizeof(int));
  int* d_shard_actual_sample_size_ptr =
      reinterpret_cast<int*>(d_shard_actual_sample_size->ptr());

  split_input_to_shard(key, d_idx_ptr, len, d_left_ptr, d_right_ptr, gpu_id);

  // fill_shard_key<<<grid_size, block_size_, 0, stream>>>(d_shard_keys_ptr,
  // key,
  //                                                     d_idx_ptr, len);
  heter_comm_kernel_->fill_shard_key(d_shard_keys_ptr, key, d_idx_ptr, len,
                                     stream);
  cudaStreamSynchronize(stream);

  cudaMemcpy(h_left, d_left_ptr, total_gpu * sizeof(int),
             cudaMemcpyDeviceToHost);
  cudaMemcpy(h_right, d_right_ptr, total_gpu * sizeof(int),
             cudaMemcpyDeviceToHost);
  // auto start1 = std::chrono::steady_clock::now();
  for (int i = 0; i < total_gpu; ++i) {
    int shard_len = h_left[i] == -1 ? 0 : h_right[i] - h_left[i] + 1;
    if (shard_len == 0) {
      continue;
    }
    /*
   comment 3
    shard_len denotes the size of keys on i-th gpu here,
    when we sample  on i-th gpu, we allocate shard_len * (1 + sample_size)
   int64_t units
    of memory, we use alloc_mem_i to denote it, the range [0,shard_len) is saved
   for the respective nodes' indexes
    and acutal sample_size.
    with nodes' indexes we could get the nodes to sample.
    since size of int64_t is 8 bits, while size of int is 4,
    the range of [0,shard_len) contains shard_len * 2 int uinits;
    The values of the first half of this range will be updated by
    the k-v map on i-th-gpu.
    The second half of this range is saved for actual sample size of each node.
    For node x,
    its sampling result is saved on the range
    [shard_len + sample_size * x,shard_len + sample_size * x +
   actual_sample_size_of_x)
    of alloc_mem_i, actual_sample_size_of_x equals ((int
   *)alloc_mem_i)[shard_len + x]
    */
    create_storage(gpu_id, i, shard_len * sizeof(int64_t),
                   shard_len * (1 + sample_size) * sizeof(int64_t));
    auto& node = path_[gpu_id][i].nodes_[0];
    cudaMemsetAsync(node.val_storage, -1, shard_len * sizeof(int),
                    node.in_stream);
  }
  // auto end1 = std::chrono::steady_clock::now();
  // auto tt = std::chrono::duration_cast<std::chrono::microseconds>(end1 -
  // start1);
  // VLOG(0)<< "create storage time  " << tt.count() << " us";
  walk_to_dest(gpu_id, total_gpu, h_left, h_right, d_shard_keys_ptr, NULL);

  for (int i = 0; i < total_gpu; ++i) {
    if (h_left[i] == -1) {
      continue;
    }
    auto& node = path_[gpu_id][i].nodes_.back();
    cudaStreamSynchronize(node.in_stream);
    platform::CUDADeviceGuard guard(resource_->dev_id(i));
    // platform::CUDADeviceGuard guard(i);
    // use the key-value map to update alloc_mem_i[0,shard_len)
    // tables_[i]->rwlock_->RDLock();
    tables_[i]->get(reinterpret_cast<int64_t*>(node.key_storage),
                    reinterpret_cast<int*>(node.val_storage),
                    h_right[i] - h_left[i] + 1,
                    resource_->remote_stream(i, gpu_id));
    // node.in_stream);
    int shard_len = h_right[i] - h_left[i] + 1;
    auto graph = gpu_graph_list[i];
    int* id_array = reinterpret_cast<int*>(node.val_storage);
    int* actual_size_array = id_array + shard_len;
    int64_t* sample_array = (int64_t*)(id_array + shard_len * 2);
    int sample_grid_size = (shard_len - 1) / dim_y + 1;
    dim3 block(parallel_sample_size, dim_y);
    dim3 grid(sample_grid_size);
    // int sample_grid_size = shard_len / block_size_ + 1;
    // VLOG(0)<<"in sample grid_size = "<<sample_grid_size<<" block_size
    // ="<<block_size_<<" device = "<<resource_->dev_id(i)<<"len = "<<len;;
    // neighbor_sample_example<<<sample_grid_size, block_size_, 0,
    //                           resource_->remote_stream(i, gpu_id)>>>(
    //     graph, res_array, actual_size_array, sample_array, sample_size,
    //     shard_len);
    neighbor_sample_example<<<grid, block, 0,
                              resource_->remote_stream(i, gpu_id)>>>(
        graph, id_array, actual_size_array, sample_array, sample_size,
        sample_status[i], shard_len, gpu_id);
  }
  /*
  for (int i = 0; i < total_gpu; ++i) {
    if (h_left[i] == -1) {
      continue;
    }
    // cudaStreamSynchronize(resource_->remote_stream(i, num));
    // tables_[i]->rwlock_->UNLock();
    platform::CUDADeviceGuard guard(i);
    //platform::CUDADeviceGuard guard(resource_->dev_id(i));
    auto& node = path_[gpu_id][i].nodes_.back();
    auto shard_len = h_right[i] - h_left[i] + 1;
    auto graph = gpu_graph_list[i];
    int* id_array = reinterpret_cast<int*>(node.val_storage);
    int* actual_size_array = id_array + shard_len;
    int64_t* sample_array = (int64_t*)(id_array + shard_len * 2);
    int sample_grid_size = (shard_len - 1) / dim_y + 1;
    dim3 block(parallel_sample_size, dim_y);
    dim3 grid(sample_grid_size);
    // int sample_grid_size = shard_len / block_size_ + 1;
    // VLOG(0)<<"in sample grid_size = "<<sample_grid_size<<" block_size
    // ="<<block_size_<<" device = "<<resource_->dev_id(i)<<"len = "<<len;;
    // neighbor_sample_example<<<sample_grid_size, block_size_, 0,
    //                           resource_->remote_stream(i, gpu_id)>>>(
    //     graph, res_array, actual_size_array, sample_array, sample_size,
    //     shard_len);
    neighbor_sample_example<<<grid, block, 0,
                              resource_->remote_stream(i, gpu_id)>>>(
        graph, id_array, actual_size_array, sample_array, sample_size,
        sample_status[i], shard_len, gpu_id);
      // neighbor_sample_example<<<grid, block, 0,
      //                         node.in_stream>>>(
      //   graph, id_array, actual_size_array, sample_array, sample_size,
      //   sample_status[i], shard_len, gpu_id);
  }
  */

  for (int i = 0; i < total_gpu; ++i) {
    if (h_left[i] == -1) {
      continue;
    }
    // auto& node = path_[gpu_id][i].nodes_.back();
    // cudaStreamSynchronize(node.in_stream);
    cudaStreamSynchronize(resource_->remote_stream(i, gpu_id));
  }
  move_neighbor_sample_result_to_source_gpu(gpu_id, total_gpu, sample_size,
                                            h_left, h_right, d_shard_vals_ptr,
                                            d_shard_actual_sample_size_ptr);
  fill_dvalues<<<grid_size, block_size_, 0, stream>>>(
      d_shard_vals_ptr, val, d_shard_actual_sample_size_ptr, actual_sample_size,
      d_idx_ptr, sample_size, len);
  for (int i = 0; i < total_gpu; ++i) {
    int shard_len = h_left[i] == -1 ? 0 : h_right[i] - h_left[i] + 1;
    if (shard_len == 0) {
      continue;
    }
    destroy_storage(gpu_id, i);
  }
  cudaStreamSynchronize(stream);
  return result;
}

NodeQueryResult* GpuPsGraphTable::graph_node_sample(int gpu_id,
                                                    int sample_size) {}

NodeQueryResult* GpuPsGraphTable::query_node_list(int gpu_id, int start,
                                                  int query_size) {
  NodeQueryResult* result = new NodeQueryResult();
  if (query_size <= 0) return result;
  int& actual_size = result->actual_sample_size;
  actual_size = 0;
  cudaMalloc((void**)&result->val, query_size * sizeof(int64_t));
  int64_t* val = result->val;
  // int dev_id = resource_->dev_id(gpu_id);
  // platform::CUDADeviceGuard guard(dev_id);
  platform::CUDADeviceGuard guard(resource_->dev_id(gpu_id));
  std::vector<int> idx, gpu_begin_pos, local_begin_pos, sample_size;
  int size = 0;
  /*
  if idx[i] = a, gpu_begin_pos[i] = p1,
  gpu_local_begin_pos[i] = p2;
  sample_size[i] = s;
  then on gpu a, the nodes of positions [p1,p1 + s) should be returned
  and saved from the p2 position on the sample_result array

  for example:
  suppose
  gpu 0 saves [0,2,4,6,8], gpu1 saves [1,3,5,7]
  start = 3, query_size = 5
  we know [6,8,1,3,5] should be returned;
  idx = [0,1]
  gpu_begin_pos = [3,0]
  local_begin_pos = [0,3]
  sample_size = [2,3]

  */
  for (int i = 0; i < gpu_graph_list.size() && query_size != 0; i++) {
    auto graph = gpu_graph_list[i];
    if (graph.node_size == 0) {
      continue;
    }
    if (graph.node_size + size > start) {
      int cur_size = min(query_size, graph.node_size + size - start);
      query_size -= cur_size;
      idx.emplace_back(i);
      gpu_begin_pos.emplace_back(start - size);
      local_begin_pos.emplace_back(actual_size);
      start += cur_size;
      actual_size += cur_size;
      sample_size.emplace_back(cur_size);
      create_storage(gpu_id, i, 1, cur_size * sizeof(int64_t));
    }
    size += graph.node_size;
  }
  for (int i = 0; i < idx.size(); i++) {
    int dev_id_i = resource_->dev_id(idx[i]);
    platform::CUDADeviceGuard guard(dev_id_i);
    // platform::CUDADeviceGuard guard(i);
    auto& node = path_[gpu_id][idx[i]].nodes_.front();
    int grid_size = (sample_size[i] - 1) / block_size_ + 1;
    node_query_example<<<grid_size, block_size_, 0,
                         resource_->remote_stream(idx[i], gpu_id)>>>(
        gpu_graph_list[idx[i]], gpu_begin_pos[i], sample_size[i],
        (int64_t*)node.val_storage);
  }

  for (int i = 0; i < idx.size(); i++) {
    cudaStreamSynchronize(resource_->remote_stream(idx[i], gpu_id));
    auto& node = path_[gpu_id][idx[i]].nodes_.front();
    cudaMemcpyAsync(reinterpret_cast<char*>(val + local_begin_pos[i]),
                    node.val_storage, node.val_bytes_len, cudaMemcpyDefault,
                    node.out_stream);
  }
  for (int i = 0; i < idx.size(); i++) {
    auto& node = path_[gpu_id][idx[i]].nodes_.front();
    cudaStreamSynchronize(node.out_stream);
  }
  return result;
}
}
};
#endif
