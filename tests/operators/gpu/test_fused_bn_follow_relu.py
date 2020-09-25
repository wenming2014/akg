# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License

from __future__ import absolute_import
import numpy as np
from gen_random import random_gaussian
from akg.utils import kernel_exec as utils
from akg.utils.result_analysis import gpu_profiling
from akg.utils.format_transform import to_tvm_nd_array
from akg.ops.poly_gpu import fused_bn_follow_relu_manual, fused_bn_follow_relu_auto

def compute_fused_bn_follow_relu(data, layout, inter_dtype, out_dtype):
    beta = data[0]
    gamma = data[1]
    bn_update = data[2]
    bn_reduce = data[3]
    xi_conv2d = data[4]

    if layout == "NCHW":
        xi_conv2d = np.transpose(xi_conv2d, axes=(0, 2, 3, 1))

    n, h, w, c = xi_conv2d.shape
    output = (xi_conv2d.astype(inter_dtype) - bn_reduce / (n * h * w)) * bn_update * gamma + beta
    output = np.maximum(output.astype(out_dtype), 0)

    if layout == "NCHW":
        output = np.transpose(output, axes=(0, 3, 1, 2))

    return output

def gen_data(in_shape, in_dtype, inter_dtype, layout, out_dtype):

    if layout == "NHWC":
        num_channel = in_shape[3]
    else:
        num_channel = in_shape[1]
    data = [np.nan] * 5
    data[0] = random_gaussian([num_channel], miu=1, sigma=0.1).astype(inter_dtype)
    data[1] = random_gaussian([num_channel], miu=1, sigma=0.1).astype(inter_dtype)
    data[2] = random_gaussian([num_channel], miu=1, sigma=0.1).astype(inter_dtype)
    data[3] = random_gaussian([num_channel], miu=1, sigma=0.1).astype(inter_dtype)
    data[4] = random_gaussian(in_shape, miu=1, sigma=0.1).astype(in_dtype)

    expect = compute_fused_bn_follow_relu(data, layout, inter_dtype, out_dtype)
    output = np.full(expect.shape, np.nan, out_dtype)

    return data, output, expect

def test_fused_bn_follow_relu(in_shape, in_dtype='float16', layout='NHWC', out_dtype='float16', poly_sch=False):

    if layout != "NHWC" and layout != "NCHW":
        raise NotImplementedError(
            'Layout not supported {} '.format(layout))

    inter_dtype = 'float32'
    inputs, output, expect = gen_data(in_shape, in_dtype, inter_dtype, layout, out_dtype)
    input_shape_list = [i.shape for i in inputs]
    input_dtype_list = [inter_dtype] * 4 + [in_dtype]
    op_attrs = [layout, out_dtype]
    if poly_sch:
        mod = utils.op_build(
            fused_bn_follow_relu_auto,
            input_shape_list,
            input_dtype_list,
            op_attrs=op_attrs,
            attrs={
                "target": "cuda"})
    else:
        mod = utils.op_build(fused_bn_follow_relu_manual, input_shape_list, input_dtype_list, op_attrs=op_attrs)

    outputs = [output]
    arglist = inputs + outputs
    output = utils.mod_launch(mod, arglist, expect=expect)
    
    res = np.allclose(output, expect, rtol=5e-03, atol=1.e-8)
    print("Test {}".format("Pass" if res else "Fail"))
    if not res:
        print("Error cuda:========================")
        print(mod.imported_modules[0].get_source())
        raise AssertionError("Test fail")
    
    inputs = to_tvm_nd_array(inputs)
    expect = to_tvm_nd_array(expect)
    gpu_profiling(mod, *inputs, expect, 400)