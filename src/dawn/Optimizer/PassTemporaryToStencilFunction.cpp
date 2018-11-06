﻿//===--------------------------------------------------------------------------------*- C++ -*-===//
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

#include "dawn/Optimizer/PassTemporaryToStencilFunction.h"
#include "dawn/IIR/Stencil.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Optimizer/AccessComputation.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Optimizer/StatementMapper.h"
#include "dawn/SIR/AST.h"
#include "dawn/SIR/ASTVisitor.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/RemoveIf.hpp"

namespace dawn {

namespace {
sir::Interval intervalToSIRInterval(iir::Interval interval) {
  return sir::Interval(interval.lowerLevel(), interval.upperLevel(), interval.lowerOffset(),
                       interval.upperOffset());
}

/// @brief some properties of the temporary being replaced
struct TemporaryFunctionProperties {
  std::shared_ptr<StencilFunCallExpr>
      stencilFunCallExpr_;        ///< stencil function call that will replace the tmp
  std::vector<int> accessIDArgs_; ///< access IDs of the args that are needed to compute the tmp
  std::shared_ptr<sir::StencilFunction>
      sirStencilFunction_; ///< sir stencil function of the tmp being created
  std::shared_ptr<FieldAccessExpr>
      tmpFieldAccessExpr_; ///< FieldAccessExpr of the tmp captured for replacement
};

///
/// @brief The LocalVariablePromotion class identifies local variables that need to be promoted to
/// temporaries because of a tmp->stencilfunction conversion
/// In the following example:
/// double a=0;
/// tmp = a*2;
/// local variable a will have to be promoted to temporary, since tmp will be evaluated on-the-fly
/// with extents
///
class LocalVariablePromotion : public ASTVisitorPostOrder, public NonCopyable {
protected:
  const std::shared_ptr<iir::StencilInstantiation>& instantiation_;
  std::unordered_set<int>& localVarAccessIDs_;
  const std::unordered_map<int, iir::Stencil::FieldInfo>& fields_;

public:
  LocalVariablePromotion(const std::shared_ptr<iir::StencilInstantiation>& instantiation,
                         const std::unordered_map<int, iir::Stencil::FieldInfo>& fields,
                         std::unordered_set<int>& localVarAccessIDs)
      : instantiation_(instantiation), localVarAccessIDs_(localVarAccessIDs), fields_(fields) {}

  virtual ~LocalVariablePromotion() override {}

  virtual bool preVisitNode(std::shared_ptr<VarAccessExpr> const& expr) override {
    // TODO if inside stencil function we should get it from stencilfun
    localVarAccessIDs_.emplace(instantiation_->getAccessIDFromExpr(expr));
    return true;
  }

  /// @brief capture a tmp computation
  virtual bool preVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {

    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      int accessID = instantiation_->getAccessIDFromExpr(expr->getLeft());
      DAWN_ASSERT(fields_.count(accessID));
      const iir::Field& field = fields_.at(accessID).field;

      if(!instantiation_->isTemporaryField(accessID))
        return false;

      if(field.getExtents().isHorizontalPointwise())
        return false;

      return true;
    }

    return false;
  }
};

/// @brief visitor that will detect assignment (i.e. computations) to a temporary,
/// it will create a sir::StencilFunction out of this computation, and replace the assignment
/// expression in the AST by a NOExpr.
class TmpAssignment : public ASTVisitorPostOrder, public NonCopyable {
protected:
  const std::shared_ptr<iir::StencilInstantiation>& instantiation_;
  sir::Interval interval_;
  std::shared_ptr<sir::StencilFunction> tmpFunction_;
  std::vector<int> accessIDs_;

  int accessID_ = -1;
  std::shared_ptr<FieldAccessExpr> tmpFieldAccessExpr_ = nullptr;

public:
  TmpAssignment(const std::shared_ptr<iir::StencilInstantiation>& instantiation,
                sir::Interval const& interval)
      : instantiation_(instantiation), interval_(interval), tmpFunction_(nullptr) {}

  virtual ~TmpAssignment() {}

  int temporaryFieldAccessID() const { return accessID_; }

  const std::vector<int>& getAccessIDs() const { return accessIDs_; }

  std::shared_ptr<FieldAccessExpr> getTemporaryFieldAccessExpr() { return tmpFieldAccessExpr_; }

  std::shared_ptr<sir::StencilFunction> temporaryStencilFunction() { return tmpFunction_; }

  bool foundTemporaryToReplace() { return (tmpFunction_ != nullptr); }

  /// @brief pre visit the node. The assignment expr visit will only continue processing the visitor
  /// for the right hand side of the =. Therefore all accesses capture here correspond to arguments
  /// for computing the tmp. They are captured as arguments of the stencil function being created
  virtual bool preVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {
    DAWN_ASSERT(tmpFunction_);
    for(int idx : expr->getArgumentMap()) {
      DAWN_ASSERT(idx == -1);
    }
    for(int off : expr->getArgumentOffset())
      DAWN_ASSERT(off == 0);

    if(!tmpFunction_->hasArg(expr->getName()) && expr != tmpFieldAccessExpr_) {

      int genLineKey = static_cast<std::underlying_type<SourceLocation::ReservedSL>::type>(
          SourceLocation::ReservedSL::SL_Generated);
      tmpFunction_->Args.push_back(
          std::make_shared<sir::Field>(expr->getName(), SourceLocation(genLineKey, genLineKey)));

      accessIDs_.push_back(instantiation_->getAccessIDFromExpr(expr));
    }
    // continue traversing
    return true;
  }

  virtual bool preVisitNode(std::shared_ptr<VarAccessExpr> const& expr) override {
    DAWN_ASSERT(tmpFunction_);
    dawn_unreachable_internal("All the var access should have been promoted to temporaries");
    return true;
  }

  /// @brief capture a tmp computation
  virtual bool preVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {
    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      // return and stop traversing the AST if the left hand of the =  is not a temporary
      int accessID = instantiation_->getAccessIDFromExpr(expr->getLeft());
      if(!instantiation_->isTemporaryField(accessID))
        return false;
      tmpFieldAccessExpr_ = std::dynamic_pointer_cast<FieldAccessExpr>(expr->getLeft());
      accessID_ = accessID;

      // otherwise we create a new stencil function
      std::string tmpFieldName = instantiation_->getNameFromAccessID(accessID_);
      tmpFunction_ = std::make_shared<sir::StencilFunction>();

      tmpFunction_->Name = tmpFieldName + "_OnTheFly";
      tmpFunction_->Loc = expr->getSourceLocation();
      tmpFunction_->Intervals.push_back(std::make_shared<sir::Interval>(interval_));

      return true;
    }
    return false;
  }
  ///@brief once the "tmp= fn(args)" has been captured, the new stencil function to compute the
  /// temporary is finalized and the assignment is replaced by a NOExpr
  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {
    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      accessID_ = instantiation_->getAccessIDFromExpr(expr->getLeft());
      if(!instantiation_->isTemporaryField(accessID_))
        return expr;

      DAWN_ASSERT(tmpFunction_);

      auto functionExpr = expr->getRight()->clone();

      auto retStmt = std::make_shared<ReturnStmt>(functionExpr);

      std::shared_ptr<BlockStmt> root = std::make_shared<BlockStmt>();
      root->push_back(retStmt);
      std::shared_ptr<AST> ast = std::make_shared<AST>(root);
      tmpFunction_->Asts.push_back(ast);

      return std::make_shared<NOPExpr>();
    }
    return expr;
  }
};

/// @brief visitor that will capture all read accesses to the temporary. The offset used to access
/// the temporary is extracted and pass to all the stencil function arguments. A new stencil
/// function instantiation is then created and the field access expression replaced by a stencil
/// function call
class TmpReplacement : public ASTVisitorPostOrder, public NonCopyable {
  // struct to store the stencil function instantiation of each parameter of a stencil function that
  // is at the same time instantiated as a stencil function
  struct NestedStencilFunctionArgs {
    int currentPos_ = 0;
    std::unordered_map<int, std::shared_ptr<iir::StencilFunctionInstantiation>> stencilFun_;
  };

protected:
  const std::shared_ptr<iir::StencilInstantiation>& instantiation_;
  std::unordered_map<int, TemporaryFunctionProperties> const& temporaryFieldAccessIDToFunctionCall_;
  const sir::Interval interval_;
  std::shared_ptr<std::vector<sir::StencilCall*>> stackTrace_;

  // the prop of the arguments of nested stencil functions
  std::stack<bool> replaceInNestedFun_;

  unsigned int numTmpReplaced_ = 0;

  std::unordered_map<std::shared_ptr<FieldAccessExpr>,
                     std::shared_ptr<iir::StencilFunctionInstantiation>>
      tmpToStencilFunctionMap_;

public:
  TmpReplacement(const std::shared_ptr<iir::StencilInstantiation>& instantiation,
                 std::unordered_map<int, TemporaryFunctionProperties> const&
                     temporaryFieldAccessIDToFunctionCall,
                 const sir::Interval sirInterval,
                 std::shared_ptr<std::vector<sir::StencilCall*>> stackTrace)
      : instantiation_(instantiation),
        temporaryFieldAccessIDToFunctionCall_(temporaryFieldAccessIDToFunctionCall),
        interval_(sirInterval), stackTrace_(stackTrace) {}

  virtual ~TmpReplacement() {}

  static std::string offsetToString(int a) {
    return ((a < 0) ? "minus" : "") + std::to_string(std::abs(a));
  }

  /// @brief create the name of a newly created stencil function associated to a tmp computations
  std::string makeOnTheFlyFunctionName(const std::shared_ptr<FieldAccessExpr>& expr) {
    return expr->getName() + "_OnTheFly" + "_i" + offsetToString(expr->getOffset()[0]) + "_j" +
           offsetToString(expr->getOffset()[1]) + "_k" + offsetToString(expr->getOffset()[2]);
  }

  unsigned int getNumTmpReplaced() { return numTmpReplaced_; }
  void resetNumTmpReplaced() { numTmpReplaced_ = 0; }

  virtual bool preVisitNode(std::shared_ptr<StencilFunCallExpr> const& expr) override {
    // we check which arguments of the stencil function are also a stencil function call
    bool doReplaceTmp = false;
    for(auto arg : expr->getArguments()) {
      if(isa<FieldAccessExpr>(*arg)) {
        int accessID = instantiation_->getAccessIDFromExpr(arg);
        if(temporaryFieldAccessIDToFunctionCall_.count(accessID)) {
          doReplaceTmp = true;
        }
      }
    }
    if(doReplaceTmp)
      replaceInNestedFun_.push(true);
    else
      replaceInNestedFun_.push(false);

    return true;
  }

  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<StencilFunCallExpr> const& expr) override {
    // at the post visit of a stencil function node, we will replace the arguments to "tmp" fields
    // by stecil function calls
    std::shared_ptr<iir::StencilFunctionInstantiation> thisStencilFun =
        instantiation_->getStencilFunctionInstantiation(expr);

    if(!replaceInNestedFun_.top())
      return expr;

    // we need to remove the previous stencil function that had "tmp" field as argument from the
    // registry, before we replace it with a StencilFunCallExpr (that computes "tmp") argument
    instantiation_->deregisterStencilFunction(thisStencilFun);
    // reset the use of nested function calls to continue using the visitor
    replaceInNestedFun_.pop();

    return expr;
  }

  /// @brief previsit the access to a temporary. Finalize the stencil function instantiation and
  /// recompute its <statement,accesses> pairs
  virtual bool preVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {
    int accessID = instantiation_->getAccessIDFromExpr(expr);
    if(!temporaryFieldAccessIDToFunctionCall_.count(accessID))
      return false;
    // TODO we need to version to tmp function generation, in case tmp is recomputed multiple times
    std::string callee = expr->getName() + "_OnTheFly";
    std::shared_ptr<iir::StencilFunctionInstantiation> stencilFun =
        instantiation_->getStencilFunctionInstantiationCandidate(callee);

    std::string fnClone = makeOnTheFlyFunctionName(expr);

    // retrieve the sir stencil function definition
    std::shared_ptr<sir::StencilFunction> sirStencilFunction =
        temporaryFieldAccessIDToFunctionCall_.at(accessID).sirStencilFunction_;

    // we create a new sir stencil function, since its name is demangled from the offsets.
    // for example, for a tmp(i+1) the stencil function is named as tmp_OnTheFly_iplus1
    std::shared_ptr<sir::StencilFunction> sirStencilFunctionInstance =
        std::make_shared<sir::StencilFunction>(*sirStencilFunction);

    sirStencilFunctionInstance->Name = fnClone;

    // TODO is this really needed, we only change the name, can we map multiple function
    // instantiations (i.e. different offsets) to the same sir stencil function
    // insert the sir::stencilFunction into the StencilInstantiation
    instantiation_->insertStencilFunctionIntoSIR(sirStencilFunctionInstance);

    std::shared_ptr<iir::StencilFunctionInstantiation> cloneStencilFun =
        instantiation_->cloneStencilFunctionCandidate(stencilFun, fnClone);

    auto& accessIDsOfArgs = temporaryFieldAccessIDToFunctionCall_.at(accessID).accessIDArgs_;

    // here we create the arguments of the stencil function instantiation.
    // find the accessID of all args, and create a new FieldAccessExpr with an offset corresponding
    // to the offset used to access the temporary
    for(auto accessID_ : (accessIDsOfArgs)) {
      std::shared_ptr<FieldAccessExpr> arg = std::make_shared<FieldAccessExpr>(
          instantiation_->getNameFromAccessID(accessID_), expr->getOffset());
      cloneStencilFun->getExpression()->insertArgument(arg);

      instantiation_->mapExprToAccessID(arg, accessID_);
    }

    for(int idx : expr->getArgumentMap()) {
      DAWN_ASSERT(idx == -1);
    }
    for(int off : expr->getArgumentOffset())
      DAWN_ASSERT(off == 0);

    const auto& argToAccessIDMap = stencilFun->ArgumentIndexToCallerAccessIDMap();
    for(auto pair : argToAccessIDMap) {
      int accessID_ = pair.second;
      cloneStencilFun->setCallerInitialOffsetFromAccessID(accessID_, expr->getOffset());
    }

    instantiation_->finalizeStencilFunctionSetup(cloneStencilFun);
    std::unordered_map<std::string, int> fieldsMap;

    const auto& arguments = cloneStencilFun->getArguments();
    for(std::size_t argIdx = 0; argIdx < arguments.size(); ++argIdx) {
      if(sir::Field* field = dyn_cast<sir::Field>(arguments[argIdx].get())) {
        int AccessID = cloneStencilFun->getCallerAccessIDOfArgField(argIdx);
        fieldsMap[field->Name] = AccessID;
      }
    }

    // recompute the list of <statement, accesses> pairs
    StatementMapper statementMapper(nullptr, instantiation_.get(), stackTrace_,
                                    *(cloneStencilFun->getDoMethod()), interval_, fieldsMap,
                                    cloneStencilFun);

    cloneStencilFun->getAST()->accept(statementMapper);

    // final checks
    cloneStencilFun->checkFunctionBindings();

    // register the FieldAccessExpr -> StencilFunctionInstantation into a map for future replacement
    // of the node in the post visit
    DAWN_ASSERT(!tmpToStencilFunctionMap_.count(expr));
    tmpToStencilFunctionMap_[expr] = cloneStencilFun;

    return true;
  }

  /// @brief replace the access to a temporary by a stencil function call expression
  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {

    int accessID = instantiation_->getAccessIDFromExpr(expr);
    // if the field access is not identified as a temporary being replaced just return without
    // substitution
    if(!temporaryFieldAccessIDToFunctionCall_.count(accessID))
      return expr;

    // TODO we need to version to tmp function generation, in case tmp is recomputed multiple times
    std::string callee = makeOnTheFlyFunctionName(expr);

    DAWN_ASSERT(tmpToStencilFunctionMap_.count(expr));

    auto stencilFunCall = tmpToStencilFunctionMap_.at(expr)->getExpression();

    numTmpReplaced_++;
    return stencilFunCall;
  }
};

} // anonymous namespace

PassTemporaryToStencilFunction::PassTemporaryToStencilFunction()
    : Pass("PassTemporaryToStencilFunction") {}

bool PassTemporaryToStencilFunction::run(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) {

  OptimizerContext* context = stencilInstantiation->getOptimizerContext();

  if(!context->getOptions().PassTmpToFunction)
    return true;

  DAWN_ASSERT(context);

  for(const auto& stencilPtr : stencilInstantiation->getStencils()) {

    std::unordered_set<int> localVarAccessIDs;

    const auto& fields = stencilPtr->getFields();

    LocalVariablePromotion localVariablePromotion(stencilInstantiation, fields, localVarAccessIDs);

    // Iterate multi-stages backwards in order to identify local variables that need to be promoted
    // to temporaries
    for(auto multiStageIt = stencilPtr->childrenRBegin();
        multiStageIt != stencilPtr->childrenREnd(); ++multiStageIt) {
      std::unordered_map<int, TemporaryFunctionProperties> temporaryFieldExprToFunction;

      for(auto stageIt = (*multiStageIt)->childrenRBegin();
          stageIt != (*multiStageIt)->childrenREnd(); ++stageIt) {

        for(auto doMethodIt = (*stageIt)->childrenRBegin();
            doMethodIt != (*stageIt)->childrenREnd(); doMethodIt++) {
          for(auto stmtAccessPairIt = (*doMethodIt)->childrenRBegin();
              stmtAccessPairIt != (*doMethodIt)->childrenREnd(); stmtAccessPairIt++) {
            const std::shared_ptr<Statement> stmt = (*stmtAccessPairIt)->getStatement();

            stmt->ASTStmt->acceptAndReplace(localVariablePromotion);
          }
        }
      }
    }
    // perform the promotion "local var"->temporary
    for(auto varID : localVarAccessIDs) {
      stencilInstantiation->promoteLocalVariableToTemporaryField(stencilPtr.get(), varID,
                                                                 stencilPtr->getLifetime(varID));
    }

    // Iterate multi-stages for the replacement of temporaries by stencil functions
    for(const auto& multiStage : stencilPtr->getChildren()) {
      std::unordered_map<int, TemporaryFunctionProperties> temporaryFieldExprToFunction;

      for(const auto& stagePtr : multiStage->getChildren()) {

        bool isATmpReplaced = false;
        for(const auto& doMethodPtr : stagePtr->getChildren()) {
          for(const auto& stmtAccessPair : doMethodPtr->getChildren()) {
            const std::shared_ptr<Statement> stmt = stmtAccessPair->getStatement();

            if(stmt->ASTStmt->getKind() != Stmt::SK_ExprStmt)
              continue;

            {
              // TODO catch a temp expr
              const iir::Interval& interval = doMethodPtr->getInterval();
              const sir::Interval sirInterval = intervalToSIRInterval(interval);

              // run the replacer visitor
              TmpReplacement tmpReplacement(stencilInstantiation, temporaryFieldExprToFunction,
                                            sirInterval, stmt->StackTrace);
              stmt->ASTStmt->acceptAndReplace(tmpReplacement);
              // flag if a least a tmp has been replaced within this stage
              isATmpReplaced = isATmpReplaced || (tmpReplacement.getNumTmpReplaced() != 0);

              if(tmpReplacement.getNumTmpReplaced() != 0) {

                iir::DoMethod tmpStmtDoMethod(interval, *stencilInstantiation);

                // WITTODO: Check if this works
                StatementMapper statementMapper(
                    nullptr, stencilInstantiation.get(), stmt->StackTrace, tmpStmtDoMethod,
                    sirInterval, stencilInstantiation->getNameToAccessIDMap(), nullptr);

                std::shared_ptr<BlockStmt> blockStmt =
                    std::make_shared<BlockStmt>(std::vector<std::shared_ptr<Stmt>>{stmt->ASTStmt});
                blockStmt->accept(statementMapper);

                DAWN_ASSERT(tmpStmtDoMethod.getChildren().size() == 1);

                std::unique_ptr<iir::StatementAccessesPair>& stmtPair =
                    *(tmpStmtDoMethod.childrenBegin());
                computeAccesses(stencilInstantiation.get(), stmtPair);

                doMethodPtr->replace(stmtAccessPair, stmtPair);
                doMethodPtr->update(iir::NodeUpdateType::level);
              }

              // find patterns like tmp = fn(args)...;
              TmpAssignment tmpAssignment(stencilInstantiation, sirInterval);
              stmt->ASTStmt->acceptAndReplace(tmpAssignment);
              if(tmpAssignment.foundTemporaryToReplace()) {
                std::shared_ptr<sir::StencilFunction> stencilFunction =
                    tmpAssignment.temporaryStencilFunction();
                std::shared_ptr<AST> ast = stencilFunction->getASTOfInterval(sirInterval);

                DAWN_ASSERT(ast);
                DAWN_ASSERT(stencilFunction);

                std::shared_ptr<StencilFunCallExpr> stencilFunCallExpr =
                    std::make_shared<StencilFunCallExpr>(stencilFunction->Name);

                // all the temporary computations captured are stored in this map of <ID, tmp
                // properties>
                // for later use of the replacer visitor
                temporaryFieldExprToFunction.emplace(
                    stencilInstantiation->getAccessIDFromExpr(
                        tmpAssignment.getTemporaryFieldAccessExpr()),
                    TemporaryFunctionProperties{stencilFunCallExpr, tmpAssignment.getAccessIDs(),
                                                stencilFunction,
                                                tmpAssignment.getTemporaryFieldAccessExpr()});

                // first instantiation of the stencil function that is inserted in the IIR as a
                // candidate stencil function
                auto stencilFun = stencilInstantiation->makeStencilFunctionInstantiation(
                    stencilFunCallExpr, stencilFunction, ast, sirInterval, nullptr);

                int argID = 0;
                for(auto accessID : tmpAssignment.getAccessIDs()) {
                  stencilFun->setCallerAccessIDOfArgField(argID++, accessID);
                }
              }
            }
          }
        }
        if(isATmpReplaced) {
          stagePtr->update(iir::NodeUpdateType::level);
        }
      }

      std::cout << "\nPASS: " << getName() << "; stencil: " << stencilInstantiation->getName();

      if(temporaryFieldExprToFunction.empty())
        std::cout << "no replacement found";

      for(auto tmpFieldPair : temporaryFieldExprToFunction) {
        int accessID = tmpFieldPair.first;
        auto tmpProperties = tmpFieldPair.second;
        if(context->getOptions().ReportPassTmpToFunction)
          std::cout << " [ replace tmp:" << stencilInstantiation->getNameFromAccessID(accessID)
                    << "; line : " << tmpProperties.tmpFieldAccessExpr_->getSourceLocation().Line
                    << " ] ";
      }
    }
    // eliminate empty stages or stages with only NOPExpr statements
    stencilPtr->childrenEraseIf(
        [](const std::unique_ptr<iir::MultiStage>& m) -> bool { return m->isEmptyOrNullStmt(); });
    for(const auto& multiStage : stencilPtr->getChildren()) {
      multiStage->childrenEraseIf(
          [](const std::unique_ptr<iir::Stage>& s) -> bool { return s->isEmptyOrNullStmt(); });
    }
    for(const auto& multiStage : stencilPtr->getChildren()) {
      multiStage->update(iir::NodeUpdateType::levelAndTreeAbove);
    }
  }

  return true;
}

} // namespace dawn
