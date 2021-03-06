//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#ifndef DAWN_OPTIMIZER_REORDERSTRATEGY_H
#define DAWN_OPTIMIZER_REORDERSTRATEGY_H

#include <memory>

namespace dawn {

class Stencil;
class DependencyGraphStage;

/// @brief Abstract class for various reodering strategies
/// @ingroup optimizer
class ReorderStrategy {
public:
  virtual ~ReorderStrategy() {}

  enum ReorderStrategyKind {
    RK_Unknown,
    RK_None,        ///< Don't perform any reordering
    RK_Greedy,      ///< Greedy fusing of the stages until max-halo boundary is reached
    RK_Partitioning ///< Use S-cut graph partitioning
  };

  /// @brief Reorder the stages of the `stencilPtr` according to the implemented strategy
  /// @returns New stencil with the reordered stages
  virtual std::shared_ptr<Stencil> reorder(const std::shared_ptr<Stencil>& stencilPtr) = 0;
};

} // namespace dawn

#endif
