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

#include "dawn/IIR/MultiStage.h"
#include "dawn/IIR/Accesses.h"
#include "dawn/IIR/DependencyGraphAccesses.h"
#include "dawn/IIR/IIRNodeIterator.h"
#include "dawn/IIR/IntervalAlgorithms.h"
#include "dawn/IIR/MultiInterval.h"
#include "dawn/IIR/Stage.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Optimizer/ReadBeforeWriteConflict.h"
#include "dawn/Optimizer/Renaming.h"
#include "dawn/Support/STLExtras.h"
#include "dawn/Support/UIDGenerator.h"

namespace dawn {
namespace iir {

MultiStage::MultiStage(StencilInstantiation& stencilInstantiation, LoopOrderKind loopOrder)
    : stencilInstantiation_(stencilInstantiation), loopOrder_(loopOrder),
      id_(UIDGenerator::getInstance()->get()) {}

std::unique_ptr<MultiStage> MultiStage::clone() const {
  auto cloneMS = make_unique<MultiStage>(stencilInstantiation_, loopOrder_);

  cloneMS->id_ = id_;
  cloneMS->derivedInfo_ = derivedInfo_;

  cloneMS->cloneChildrenFrom(*this);
  return cloneMS;
}

std::vector<std::unique_ptr<MultiStage>>
MultiStage::split(std::deque<MultiStage::SplitIndex>& splitterIndices,
                  LoopOrderKind lastLoopOrder) {

  std::vector<std::unique_ptr<MultiStage>> newMultiStages;

  int curStageIndex = 0;
  auto curStageIt = children_.begin();
  std::deque<int> curStageSplitterIndices;

  newMultiStages.push_back(make_unique<MultiStage>(stencilInstantiation_, lastLoopOrder));

  for(std::size_t i = 0; i < splitterIndices.size(); ++i) {
    SplitIndex& splitIndex = splitterIndices[i];

    if(splitIndex.StageIndex == curStageIndex) {

      curStageSplitterIndices.push_back(splitIndex.StmtIndex);
      newMultiStages.push_back(
          make_unique<MultiStage>(stencilInstantiation_, splitIndex.LowerLoopOrder));
      lastLoopOrder = splitIndex.LowerLoopOrder;
    }

    if(i == (splitterIndices.size() - 1) || splitIndex.StageIndex != curStageIndex) {
      if(!curStageSplitterIndices.empty()) {

        // Split the current stage (we assume the graphs are assigned in the stage splitter pass)
        auto newStages = (**curStageIt).split(curStageSplitterIndices, nullptr);

        // Move the new stages to the new MultiStage
        auto newMultiStageRIt = newMultiStages.rbegin();
        for(auto newStagesRIt = newStages.rbegin(); newStagesRIt != newStages.rend();
            ++newStagesRIt, ++newMultiStageRIt)
          (*newMultiStageRIt)->insertChild(*(std::make_move_iterator(newStagesRIt)));

        curStageSplitterIndices.clear();
      } else {
        // No split in this stage, just move it to the current multi-stage
        newMultiStages.back()->insertChild(std::move(*curStageIt));
      }

      if(i != (splitterIndices.size() - 1))
        newMultiStages.push_back(make_unique<MultiStage>(stencilInstantiation_, lastLoopOrder));

      // Handle the next stage
      curStageIndex++;
      curStageIt++;
    }
  }

  return newMultiStages;
}

std::shared_ptr<DependencyGraphAccesses>
MultiStage::getDependencyGraphOfInterval(const Interval& interval) const {
  auto dependencyGraph = std::make_shared<DependencyGraphAccesses>(&stencilInstantiation_);
  std::for_each(children_.begin(), children_.end(), [&](const std::unique_ptr<Stage>& stagePtr) {
    if(interval.overlaps(stagePtr->getEnclosingExtendedInterval()))
      std::for_each(stagePtr->childrenBegin(), stagePtr->childrenEnd(),
                    [&](const Stage::DoMethodSmartPtr_t& DoMethodPtr) {
                      dependencyGraph->merge(DoMethodPtr->getDependencyGraph().get());
                    });
  });
  return dependencyGraph;
}

std::shared_ptr<DependencyGraphAccesses> MultiStage::getDependencyGraphOfAxis() const {
  auto dependencyGraph = std::make_shared<DependencyGraphAccesses>(&stencilInstantiation_);
  std::for_each(children_.begin(), children_.end(), [&](const std::unique_ptr<Stage>& stagePtr) {
    std::for_each(stagePtr->childrenBegin(), stagePtr->childrenEnd(),
                  [&](const Stage::DoMethodSmartPtr_t& DoMethodPtr) {
                    dependencyGraph->merge(DoMethodPtr->getDependencyGraph().get());
                  });
  });
  return dependencyGraph;
}

iir::Cache& MultiStage::setCache(iir::Cache::CacheTypeKind type, iir::Cache::CacheIOPolicy policy,
                                 int AccessID, const Interval& interval,
                                 boost::optional<iir::Cache::window> w) {
  return derivedInfo_.caches_
      .emplace(AccessID, iir::Cache(type, policy, AccessID, boost::optional<Interval>(interval), w))
      .first->second;
}

iir::Cache& MultiStage::setCache(iir::Cache::CacheTypeKind type, iir::Cache::CacheIOPolicy policy,
                                 int AccessID) {
  return derivedInfo_.caches_
      .emplace(AccessID, iir::Cache(type, policy, AccessID, boost::optional<Interval>(),
                                    boost::optional<iir::Cache::window>()))
      .first->second;
}

std::vector<std::unique_ptr<DoMethod>> MultiStage::computeOrderedDoMethods() const {
  auto intervals_set = getIntervals();
  std::vector<Interval> intervals_v;
  std::copy(intervals_set.begin(), intervals_set.end(), std::back_inserter(intervals_v));

  // compute the partition of the intervals
  auto partitionIntervals = Interval::computePartition(intervals_v);
  if((getLoopOrder() == LoopOrderKind::LK_Backward))
    std::reverse(partitionIntervals.begin(), partitionIntervals.end());

  std::vector<std::unique_ptr<DoMethod>> orderedDoMethods;

  for(auto interval : partitionIntervals) {

    for(const auto& doMethod : iterateIIROver<DoMethod>(*this)) {
      if(doMethod->getInterval().overlaps(interval)) {
        std::unique_ptr<DoMethod> partitionedDoMethod = doMethod->clone();

        partitionedDoMethod->setInterval(interval);
        orderedDoMethods.push_back(std::move(partitionedDoMethod));
        // there should not be two do methods in the same stage with overlapping intervals
        continue;
      }
    }
  }

  return orderedDoMethods;
}

MultiInterval MultiStage::computeReadAccessInterval(int accessID) const {

  std::vector<std::unique_ptr<DoMethod>> orderedDoMethods = computeOrderedDoMethods();

  MultiInterval writeInterval;
  MultiInterval writeIntervalPre;
  MultiInterval readInterval;

  for(const auto& doMethod : orderedDoMethods) {
    for(const auto& statementAccesssPair : doMethod->getChildren()) {
      const Accesses& accesses = *statementAccesssPair->getAccesses();
      if(accesses.hasWriteAccess(accessID)) {
        writeIntervalPre.insert(doMethod->getInterval());
      }
    }
  }

  for(const auto& doMethod : orderedDoMethods) {
    for(const auto& statementAccesssPair : doMethod->getChildren()) {
      const Accesses& accesses = *statementAccesssPair->getAccesses();
      // indepdently of whether the statement has also a write access, if there is a read
      // access, it should happen in the RHS so first
      if(accesses.hasReadAccess(accessID)) {
        MultiInterval interv;

        Extents const& readAccessExtent = accesses.getReadAccess(accessID);
        boost::optional<Extent> readAccessInLoopOrder = readAccessExtent.getVerticalLoopOrderExtent(
            getLoopOrder(), Extents::VerticalLoopOrderDir::VL_InLoopOrder, false);
        Interval computingInterval = doMethod->getInterval();
        if(readAccessInLoopOrder.is_initialized()) {
          interv.insert(computingInterval.extendInterval(*readAccessInLoopOrder));
        }
        if(!writeIntervalPre.empty()) {
          interv.substract(writeIntervalPre);
        }

        if(readAccessExtent.hasVerticalCenter()) {
          auto centerAccessInterval = substract(computingInterval, writeInterval);
          interv.insert(centerAccessInterval);
        }

        boost::optional<Extent> readAccessCounterLoopOrder =
            readAccessExtent.getVerticalLoopOrderExtent(
                getLoopOrder(), Extents::VerticalLoopOrderDir::VL_CounterLoopOrder, false);

        if(readAccessCounterLoopOrder.is_initialized()) {
          interv.insert(computingInterval.extendInterval(*readAccessCounterLoopOrder));
        }

        readInterval.insert(interv);
      }
      if(accesses.hasWriteAccess(accessID)) {
        writeInterval.insert(doMethod->getInterval());
      }
    }
  }

  return readInterval;
}

boost::optional<Interval>
MultiStage::computeEnclosingAccessInterval(const int accessID,
                                           const bool mergeWithDoInterval) const {
  boost::optional<Interval> interval;
  for(auto const& stage : children_) {
    boost::optional<Interval> doInterval =
        stage->computeEnclosingAccessInterval(accessID, mergeWithDoInterval);

    if(doInterval) {
      if(interval)
        (*interval).merge(*doInterval);
      else
        interval = doInterval;
    }
  }
  return interval;
}

std::unordered_set<Interval> MultiStage::getIntervals() const {
  std::unordered_set<Interval> intervals;

  for(const auto& doMethodPtr : iterateIIROver<DoMethod>(*this)) {
    intervals.insert(doMethodPtr->getInterval());
  }
  return intervals;
}

Interval MultiStage::getEnclosingInterval() const {
  DAWN_ASSERT(!children_.empty());
  Interval interval = (*children_.begin())->getEnclosingInterval();

  for(auto it = std::next(children_.begin()), end = children_.end(); it != end; ++it)
    interval.merge((*children_.begin())->getEnclosingInterval());

  return interval;
}

boost::optional<Interval> MultiStage::getEnclosingAccessIntervalTemporaries() const {
  boost::optional<Interval> interval;
  // notice we dont use here the fields of getFields() since they contain the enclosing of all the
  // extents and intervals of all stages and it would give larger intervals than really required
  // inspecting the extents and intervals of individual stages
  for(const auto& stagePtr : children_) {
    for(const auto& fieldPair : stagePtr->getFields()) {
      const Field& field = fieldPair.second;
      int AccessID = fieldPair.first;

      if(!stencilInstantiation_.isTemporaryField(AccessID))
        continue;

      if(!interval.is_initialized()) {
        interval = boost::make_optional(field.computeAccessedInterval());
      } else {
        interval->merge(field.computeAccessedInterval());
      }
    }
  }

  return interval;
}

std::unordered_map<int, Field> MultiStage::computeFieldsOnTheFly() const {
  std::unordered_map<int, Field> fields;

  for(const auto& stagePtr : children_) {
    mergeFields(stagePtr->getFields(), fields, stagePtr->getExtents());
  }

  return fields;
}

const std::unordered_map<int, Field>& MultiStage::getFields() const { return derivedInfo_.fields_; }

void MultiStage::updateFromChildren() {
  derivedInfo_.fields_.clear();
  for(const auto& stagePtr : children_) {
    mergeFields(stagePtr->getFields(), derivedInfo_.fields_,
                boost::make_optional(stagePtr->getExtents()));
  }
}

const Field& MultiStage::getField(int accessID) const {
  DAWN_ASSERT(derivedInfo_.fields_.count(accessID));
  return derivedInfo_.fields_.at(accessID);
}

const iir::Cache& MultiStage::getCache(const int accessID) const {
  DAWN_ASSERT(derivedInfo_.caches_.count(accessID));
  return derivedInfo_.caches_.at(accessID);
}

void MultiStage::renameAllOccurrences(int oldAccessID, int newAccessID) {
  for(auto stageIt = childrenBegin(); stageIt != childrenEnd(); ++stageIt) {
    Stage& stage = (**stageIt);
    for(const auto& doMethodPtr : stage.getChildren()) {
      const DoMethod& doMethod = *doMethodPtr;
      renameAccessIDInStmts(&stencilInstantiation_, oldAccessID, newAccessID,
                            doMethod.getChildren());
      renameAccessIDInAccesses(&stencilInstantiation_, oldAccessID, newAccessID,
                               doMethod.getChildren());
    }

    stage.update(NodeUpdateType::levelAndTreeAbove);
  }
}

bool MultiStage::isEmptyOrNullStmt() const {
  for(const auto& stage : getChildren()) {
    if(!(stage)->isEmptyOrNullStmt()) {
      return false;
    }
  }
  return true;
}

} // namespace iir
} // namespace dawn