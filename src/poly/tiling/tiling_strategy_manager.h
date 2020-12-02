/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef POLY_TILING_STRATEGY_MANAGER_H_
#define POLY_TILING_STRATEGY_MANAGER_H_

#include <iostream>
#include <algorithm>
#include <deque>

#include "poly/tiling/tiling_analyzer.h"

namespace akg {
namespace ir {
namespace poly {
class TilingStrategy {
 public:
  explicit TilingStrategy(const TilingAnalyzer *a) : analyzer_(a), target_(a->scop_info_.user_config_.GetTarget()) {}
  ~TilingStrategy() {}
  virtual void AddDavinciConstraint(){};
  virtual void AddGpuConstraint(){};

  std::string interested_attr_key;

 protected:
  const TilingAnalyzer *analyzer_;
  std::string target_;
  std::unordered_map<TileAxis *, std::vector<AttrInfo>> GetInterestedInfo(const std::string &attr_key,
                                                                          bool match_whole_word = true) {
    std::unordered_map<TileAxis *, std::vector<AttrInfo>> result;
    std::vector<TileAxis *> axes =
      match_whole_word ? analyzer_->GetAxesOfAttr(attr_key) : analyzer_->GetAxesContainsAttr(attr_key);
    for (auto a : axes) {
      std::vector<AttrInfo> info;
      for (const auto &attr : a->attrs) {
        if ((match_whole_word && attr.attr_key != attr_key) ||
            (!match_whole_word && attr.attr_key.find(attr_key) == std::string::npos)) {
          continue;
        }
        info.emplace_back(attr);
      }
      result[a] = info;
    }
    return result;
  }

  // gpu configs
  int64_t warp_sizes_ = 32;
  int64_t max_num_blocks_ = 256 * 256;
  int64_t max_num_threads_ = 1024;
  size_t max_dim_ = 3;
  int64_t max_elem_per_thread_ = 1024;
};

class TilingStrategyManager {
 public:
  TilingStrategyManager() {}
  ~TilingStrategyManager() {}

  void SetStrategies(std::vector<TilingStrategy *> strategies) {
    this->strategies_.assign(strategies.begin(), strategies.end());
  }

  void Execute() {
    for (auto strategy : this->strategies_) {
      strategy->AddDavinciConstraint();
    }
  }

  void ExecuteGpu() {
    for (auto strategy : this->strategies_) {
      strategy->AddGpuConstraint();
    }
  }

 private:
  std::vector<TilingStrategy *> strategies_;
};

class GpuDmaAnalysisStrategy : public TilingStrategy {
 public:
  explicit GpuDmaAnalysisStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~GpuDmaAnalysisStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
};

class CustomTilingStrategy : public TilingStrategy {
 public:
  explicit CustomTilingStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~CustomTilingStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = "CUSTOM";
};

class ConflictTreeRangeStrategy : public TilingStrategy {
 public:
  explicit ConflictTreeRangeStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ConflictTreeRangeStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
};

class ModStrategy : public TilingStrategy {
 public:
  explicit ModStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ModStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  void GpuScalarBroadcastStrategy();
  void GpuVectorBroadcastStrategy();
  int64_t fused_size_{1};
  std::string interested_attr_key = AT_MOD;
};

// These strategies aim to deal with special insn in Davinci core.
class CastStrategy : public TilingStrategy {
 public:
  explicit CastStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~CastStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
  void MarkDataSize() {
    auto interested_info = GetInterestedInfo(interested_attr_key);
    for (auto it : interested_info) {
      TileAxis *axis = it.first;
      for (const auto &attr : it.second) {
        std::vector<std::string> src_dst = akg::common::Split(attr.attr_value, "->");
        CHECK_EQ(src_dst.size(), 2U);

        std::vector<std::string> src_list = akg::common::Split(src_dst[0], ",");
        CHECK_GE(src_list.size(), 1U);
        for (const auto &src : src_list) {
          std::vector<std::string> src_info = akg::common::Split(src, ":");
          CHECK_EQ(src_info.size(), 2U);
          CHECK_NE(src_info[1], "");
          axis->data_size[src_info[0]].emplace_back(static_cast<int>(std::strtol(src_info[1].c_str(), nullptr, 10)));
        }

        std::vector<std::string> dst_info = akg::common::Split(src_dst[1], ":");
        CHECK_EQ(dst_info.size(), 2U);
        CHECK_NE(dst_info[1], "");
        axis->data_size[dst_info[0]].emplace_back(static_cast<int>(std::strtol(dst_info[1].c_str(), nullptr, 10)));
      }
    }
  }

  std::string interested_attr_key = AT_CAST;
};

class ReduceStrategy : public TilingStrategy {
 public:
  explicit ReduceStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ReduceStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
  void DealWith4DFusedReduce(const std::vector<akg::ir::poly::TileAxis *> &reduce_axes);

  void SimpleStrategyOnGpu();

  // Used by setting scop_info.enable_akg_reduce_lib.
  void AkgReduceLibStrategyOnGpu();

  bool UseRegisterMem();

  // For this special case, we have tiling constraint on axis to calculate correct isl_footprint_box.
  void DealWith4DFusedReduce();

  // For post reduce case, we should identify and disable atomic add for reduce axes.
  void DealWithPostReduceTensors();

  std::vector<TileAxis *> reduce_axes_;
  std::vector<TileAxis *> injective_axes_;
  bool all_reduce_{false};
  bool has_transpose_{false};
};

class VectorizedStrategy : public TilingStrategy {
 public:
  explicit VectorizedStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~VectorizedStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
};

class DmaAlignStrategy : public TilingStrategy {
 public:
  explicit DmaAlignStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~DmaAlignStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_ALIGN;
};

class TensorOfTensorStrategy : public TilingStrategy {
 public:
  explicit TensorOfTensorStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~TensorOfTensorStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
};

class PassDownAttrStrategy : public TilingStrategy {
 public:
  explicit PassDownAttrStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~PassDownAttrStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();
};

class DynamicShapeLimitStrategy : public TilingStrategy {
 public:
  explicit DynamicShapeLimitStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~DynamicShapeLimitStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = "DYN_SHAPE_LIMIT";
};

class ShiftAxisStrategy : public TilingStrategy {
 public:
  explicit ShiftAxisStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ShiftAxisStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_SHIFT;
};

class ModShiftAxisStrategy : public TilingStrategy {
 public:
  explicit ModShiftAxisStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ModShiftAxisStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_MODSHIFT;
};

class DynamicBoundStrategy : public TilingStrategy {
 public:
  explicit DynamicBoundStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~DynamicBoundStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_DYNAMIC_BOUND;
};

class ConvStrategy : public TilingStrategy {
 public:
  explicit ConvStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~ConvStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_CONV;

  std::unordered_map<std::string, Expr> conv_info_{};
  air::arith::Analyzer arith_ana_;

  void RestrainH(TileAxis *axis);
  void RestrainW(TileAxis *axis);
};

class GemmStrategy : public TilingStrategy {
 public:
  explicit GemmStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~GemmStrategy() {}
  void AddDavinciConstraint();
  void AddGpuConstraint();

  std::string interested_attr_key = AT_GEMM;
};

class GpuStrategy : public TilingStrategy {
 public:
  explicit GpuStrategy(const TilingAnalyzer *a) : TilingStrategy(a) {}
  ~GpuStrategy() {}
  enum Template {
    DEFAULT = 0,
    PURE_ELEM,
    REDUCTION,
    ALL_REDUCE,
    BITWISE_REDUCTION,
    MATMUL,
    TRANSPOSE_OP,
    CUSTOM_CONFIG,
    TEMPLATE_BULK
  };
  void AddDavinciConstraint();
  void AddGpuConstraint();

 private:
  void DetermineTemplate();
  void AdjustThreadMappingLimit();

  // Step 0. Init mapping limit according to operation type.
  void InitMappingLimit();

  // Step 1. Collect axes and sort them from inner to outer
  void BuildAxesQueue();

  /*
   * Step 2. Tile inner axes first and map them to threads, and then tile outer axis and map the rest of them to blocks.
   * e.g.
   *   input: add op with shape [2, 32, 256, 32, 32]
   *   tile size: [1, 1, 1, 32, 32]
   *   band after tile:  [2, 32, 256, 1, 1] -> child [1, 1, 1, 32, 32]
   *   mapping: [2(b0), 32(b1), 4(b2), 1, 1] -> child [1, 1, 1, 32(t1), 32(t0)]
   */
  void InnerThreadOuterBlock();

  int64_t GetThreadSize(const int64_t rest_threads, size_t inner_dim, const int64_t shape, const int64_t item);
  int64_t TileAfterThreadMapping(TileAxis *axis, size_t inner_dim, int64_t thread_size, const int64_t item);

  // Step 3. Transform list of integer into string mapping config.
  void SetMappingConfig();

  Template template_{Template::DEFAULT};
  bool is_reduce_op_[TEMPLATE_BULK] = {false, false, true, true, true, false};

  std::deque<std::pair<TileAxis *, int64_t>> pending_axes_;
  std::vector<int64_t> block_limit_;
  std::vector<int64_t> thread_limit_;
  std::vector<int64_t> block_cfg_;
  std::vector<int64_t> thread_cfg_;
  int block_count_{0};  // number of mapped blocks
  int64_t elem_per_thread_[3]{SpItemPerThread::AUTO};
  int64_t min_elem_for_io_bound_ = 2;
  std::unordered_map<int, std::string> template_map_ = {{0, "DEFAULT"},     {1, "PURE_ELEM"},         {2, "REDUCTION"},
                                                        {3, "ALL_REDUCE"},  {4, "BITWISE_REDUCTION"}, {5, "MATMUL"},
                                                        {6, "TRANSPOSE_OP"}};
};

class MulticoreStrategy {
 public:
  MulticoreStrategy(TileCandidate &cand, const std::string log_file)
      : cand_(cand), logger_(TileLogger::GetInstance(log_file)) {}
  ~MulticoreStrategy() {}
  int64_t AdjustTilingAccordingToMulticoreConstraint(TileAxis *axis, int64_t tiling_factor);

 private:
  TileCandidate &cand_;
  TileLogger &logger_;
  std::pair<int, int> GetProposalRangeForFullMulticore(TileAxis *axis);
  int GetProposalCoreNum();
};

class TilingPriorityScorer {
 public:
  TilingPriorityScorer(TilingAnalyzer &analyzer)
      : analyzer_(analyzer), logger_(TileLogger::GetInstance(analyzer.logger_.GetDumpDir())) {}
  ~TilingPriorityScorer() {}

  /*
   * Compute a total score of priority for each tile axis considering all related features and corresponding weights.
   * Tile axis with higher score will have higher tiling priority (i.e. have more memory space).
   * Note that score of each feature is standardlised into range [1, tile_axis_size].
   */
  void SetPriorityByScoring();

  void SetParallelismWeight(const int parallelism) { weight_.parallelism = parallelism; }
  void SetVectorizationWeight(const int vectorization) { weight_.vectorization = vectorization; }
  void SetDataReuseWeight(const int tile_dependency) { weight_.tile_dependency = tile_dependency; }

 private:
  TilingAnalyzer &analyzer_;
  TileLogger &logger_;

  /*
   * Weight parameters for each feature in priority score model.
   * Initial weights are set empirically and changing they can support micro-tuning.
   */
  struct Weight {
    int parallelism{1};  // get lowest weight because coincident may not always trustable
    int tile_dependency{2};
    int vectorization{3};
    int Sum() { return parallelism + vectorization + tile_dependency; }
  } weight_;

  /*
   * Parallelism is computed by checking coincident value in schedule tree for corresponding axis.
   * If an axis can be parallelised, the parallelism score is 0; otherwise it is 1.
   */
  std::vector<double> ComputeParallelism(std::vector<TileAxis *> tile_axes);

  /*
   * Tile dependency describes the relationship between tile axes: if more tile axes are dependended on one tile axis,
   * this tile axis will have higher tile dependency score and gets higher priority during tiling.
   * For example, reduce axis is usually depended by other axes and thus it should be put into local buffer first.
   */
  std::vector<double> ComputeTileDependency(std::vector<TileAxis *> tile_axes);

  /*
   * Vectorization is computed by accumulating the dimension index of corresponding axis on each buffer.
   * If an axis is related with more innermost dimensions of different buffers, the vectorization score is higher.
   */
  std::vector<double> ComputeVectorization(std::vector<TileAxis *> tile_axes);

  /*
   * Normalize data to range [1, range_max].
   * `range_max` is usually set to the size of tile axes that need to determine priority.
   */
  std::vector<double> MinMaxScaler(std::vector<double> data, int range_max = 1) {
    auto min = *min_element(data.begin(), data.end());
    auto max = *max_element(data.begin(), data.end());
    std::stringstream ss;
    ss << "Min: " << min << ", Max: " << max;
    logger_.AppendLog(DO_TILING, ss);
    std::vector<double> scaled_data(data.size(), 1);
    if (max - min == 0) {
      return scaled_data;
    }
    for (int i = 0; i < static_cast<int>(data.size()); ++i) {
      auto old_d = data[i];
      ss << "Orginal data: " << old_d;
      auto new_d = (old_d - min) / (max - min);
      new_d = range_max > 1 ? (new_d * (range_max - 1) + 1) : new_d;
      ss << " -> Scaled data: " << new_d;
      scaled_data[i] = new_d;
      logger_.AppendLog(DO_TILING, ss);
    }
    return scaled_data;
  }

  std::vector<TileAxis *> GetBandTileAxes(int band_idx) {
    std::vector<TileAxis *> tile_axes;
    auto Collect = [&tile_axes, band_idx](TileAxis *axis) {
      if (axis->index == band_idx) {
        tile_axes.emplace_back(axis);
      }
    };
    analyzer_.ForEachAxisTopDown(Collect);
    return tile_axes;
  }
};
}  // namespace poly
}  // namespace ir
}  // namespace akg
#endif  // POLY_TILING_STRATEGY_MANAGER_H_
