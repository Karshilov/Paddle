/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#if defined(PADDLE_WITH_XPU_KP)
#include "xpu/kernel/cluster_header.h"
#include "xpu/kernel/debug.h"
#include "xpu/kernel/math.h"
#endif

namespace optimizer_config {

#if defined(PADDLE_WITH_CUDA)

__constant__ float nonclk_coeff = 0.1;
__constant__ float clk_coeff = 1;

__constant__ float min_bound = -10;
__constant__ float max_bound = 10;
__constant__ float learning_rate = 0.05;
__constant__ float initial_g2sum = 3.0;
__constant__ float initial_range = 0;

__constant__ float mf_create_thresholds = 10;
__constant__ float mf_learning_rate = 0.05;
__constant__ float mf_initial_g2sum = 3.0;
__constant__ float mf_initial_range = 1e-4;
__constant__ float mf_min_bound = -10;
__constant__ float mf_max_bound = 10;

#elif defined(PADDLE_WITH_XPU_KP)

_global_ptr_ float* nonclk_coeff;
_global_ptr_ float* clk_coeff;

_global_ptr_ float* min_bound;
_global_ptr_ float* max_bound;
_global_ptr_ float* learning_rate;
_global_ptr_ float* initial_g2sum;
_global_ptr_ float* initial_range;

_global_ptr_ float* mf_create_thresholds;
_global_ptr_ float* mf_learning_rate;
_global_ptr_ float* mf_initial_g2sum;
_global_ptr_ float* mf_initial_range;
_global_ptr_ float* mf_min_bound;
_global_ptr_ float* mf_max_bound;

#endif
}  // namespace optimizer_config
