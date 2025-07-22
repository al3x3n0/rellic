/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/ASTBuilder.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Sema.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/Util.h"
#include "rellic/Exception.h"

namespace rellic {

enum CExprPrecedence : unsigned {
  Value = 0U,
  SpecialOp,
  UnaryOp,
  BinaryOp = UnaryOp + clang::UO_LNot + 1U,
  CondOp = BinaryOp + clang::BO_Comma + 1U
};

namespace {

static unsigned GetOperatorPrecedence(clang::UnaryOperatorKind opc) {
  return static_cast<unsigned>(CExprPrecedence::UnaryOp) +
         static_cast<unsigned>(opc);
}

static unsigned GetOperatorPrecedence(clang::BinaryOperatorKind opc) {
  return static_cast<unsigned>(CExprPrecedence::BinaryOp) +
         static_cast<unsigned>(opc);
}

static unsigned GetOperatorPrecedence(clang::Expr *op) {
  if (auto cast = clang::dyn_cast<clang::ImplicitCastExpr>(op)) {
    return GetOperatorPrecedence(cast->getSubExpr());
  }

  if (clang::isa<clang::DeclRefExpr>(op) ||
      clang::isa<clang::IntegerLiteral>(op) ||
      clang::isa<clang::FloatingLiteral>(op) ||
      clang::isa<clang::InitListExpr>(op) ||
      clang::isa<clang::CompoundLiteralExpr>(op) ||
      clang::isa<clang::ParenExpr>(op) ||
      clang::isa<clang::StringLiteral>(op)) {
    return CExprPrecedence::Value;
  }

  if (clang::isa<clang::MemberExpr>(op) ||
      clang::isa<clang::ArraySubscriptExpr>(op) ||
      clang::isa<clang::CallExpr>(op)) {
    return CExprPrecedence::SpecialOp;
  }

  if (clang::isa<clang::CStyleCastExpr>(op)) {
    return CExprPrecedence::UnaryOp;
  }

  if (auto uo = clang::dyn_cast<clang::UnaryOperator>(op)) {
    return GetOperatorPrecedence(uo->getOpcode());
  }

  if (auto bo = clang::dyn_cast<clang::BinaryOperator>(op)) {
    return GetOperatorPrecedence(bo->getOpcode());
  }

  if (clang::isa<clang::ConditionalOperator>(op)) {
    return CExprPrecedence::CondOp;
  }

  THROW() << "Unknown clang::Expr " << ClangThingToString(op);

  return 0U;
}

}  // namespace

ASTBuilder::ASTBuilder(clang::ASTUnit &unit)
    : unit(unit),
      ctx(unit.getASTContext()),
      sema(unit.getSema()) {}

clang::QualType ASTBuilder::GetLeastIntTypeForBitWidth(unsigned size,
                                                       unsigned sign) {
  auto result{ctx.getIntTypeForBitwidth(size, sign)};
  if (!result.isNull()) {
    return result;
  }
  auto &ti{ctx.getTargetInfo()};
  auto target_type{ti.getLeastIntTypeByWidth(size, sign)};

  CHECK_THROW(target_type != clang::TargetInfo::IntType::NoInt)
      << "Failed to infer clang::TargetInfo::IntType for bitwidth: " << size;

  result = ctx.getIntTypeForBitwidth(ti.getTypeWidth(target_type), sign);

  CHECK_THROW(!result.isNull())
      << "Failed to infer clang::QualType for bitwidth: " << size;

  return result;
}

clang::QualType ASTBuilder::GetLeastRealTypeForBitWidth(unsigned size) {
  auto result{ctx.getRealTypeForBitwidth(size, clang::FloatModeKind::Float)};
  if (!result.isNull()) {
    return result;
  }

  if (size <= ctx.getTypeSize(ctx.FloatTy)) {
    return ctx.FloatTy;
  }

  if (size <= ctx.getTypeSize(ctx.DoubleTy)) {
    return ctx.DoubleTy;
  }

  if (size <= ctx.getTypeSize(ctx.LongDoubleTy)) {
    return ctx.LongDoubleTy;
  }

  THROW() << "Failed to infer real clang::QualType for bitwidth: " << size;

  return clang::QualType();
}

clang::IntegerLiteral *ASTBuilder::CreateIntLit(llvm::APSInt val) {
  auto sign{val.isSigned()};
  auto value_size{val.getBitWidth()};
  // Infer integer type wide enough to accommodate the value,
  // with `unsigned int` being the smallest type allowed.
  clang::QualType type;
  if (value_size <= ctx.getIntWidth(ctx.IntTy)) {
    type = sign ? ctx.IntTy : ctx.UnsignedIntTy;
  } else {
    type = GetLeastIntTypeForBitWidth(value_size, sign);
  }
  // Extend the literal value based on it's sign if we have a
  // mismatch between the bit width of the value and inferred type.
  auto type_size{ctx.getIntWidth(type)};
  if (val.getBitWidth() != type_size && val.getMinSignedBits() < type_size) {
    val = val.extOrTrunc(type_size);
  }
  // Clang does this check in the `clang::IntegerLiteral::Create`, but
  // we've had the calls with mismatched bit widths succeed before so
  // just in case we have ours here too.
  CHECK_EQ(val.getBitWidth(), ctx.getIntWidth(type));
  return clang::IntegerLiteral::Create(ctx, val, type, clang::SourceLocation());
}

clang::CharacterLiteral *ASTBuilder::CreateCharLit(llvm::APInt val) {
  CHECK(val.getBitWidth() == 8U);
  return new (ctx) clang::CharacterLiteral(
      val.getLimitedValue(), clang::CharacterLiteral::CharacterKind::Ascii,
      ctx.IntTy, clang::SourceLocation());
}

clang::CharacterLiteral *ASTBuilder::CreateCharLit(unsigned val) {
  return new (ctx) clang::CharacterLiteral(
      val, clang::CharacterLiteral::CharacterKind::Ascii, ctx.IntTy,
      clang::SourceLocation());
}

clang::StringLiteral *ASTBuilder::CreateStrLit(std::string val) {
  auto type{ctx.getStringLiteralArrayType(ctx.CharTy, val.size())};
  return clang::StringLiteral::Create(
      ctx, val, clang::StringLiteral::StringKind::Ordinary,
      /*Pascal=*/false, type, clang::SourceLocation());
}

clang::Expr *ASTBuilder::CreateFPLit(llvm::APFloat val) {
  auto size{llvm::APFloat::getSizeInBits(val.getSemantics())};
  auto type{GetLeastRealTypeForBitWidth(size)};
  CHECK_THROW(!type.isNull()) << "Unable to infer type for given value.";
  if (val.isNaN()) {
    std::vector<clang::Expr *> args{CreateStrLit("")};
    return CreateBuiltinCall(clang::Builtin::BI__builtin_nan, args);
  } else if (val.isInfinity()) {
    std::vector<clang::Expr *> args;
    auto inf{CreateBuiltinCall(clang::Builtin::BI__builtin_inf, args)};
    if (val.isNegative()) {
      return CreateUnaryOp(clang::UO_Minus, inf);
    } else {
      return inf;
    }
  }
  return clang::FloatingLiteral::Create(ctx, val, /*isexact=*/true, type,
                                        clang::SourceLocation());
}

clang::Expr *ASTBuilder::CreateNull() {
  auto type{ctx.UnsignedIntTy};
  auto val{llvm::APInt::getNullValue(ctx.getTypeSize(type))};
  auto lit{CreateIntLit(val)};
  return CreateCStyleCast(ctx.VoidPtrTy, lit);
}

clang::Expr *ASTBuilder::CreateUndefInteger(clang::QualType type) {
  auto val{llvm::APInt::getNullValue(ctx.getTypeSize(type))};
  auto lit{CreateIntLit(val)};
  return lit;
}

clang::Expr *ASTBuilder::CreateUndefPointer(clang::QualType type) {
  auto null{CreateNull()};
  auto cast{CreateCStyleCast(ctx.getPointerType(type), null)};
  return cast;
}

clang::IdentifierInfo *ASTBuilder::CreateIdentifier(std::string name) {
  std::string str{""};
  for (auto chr : name) {
    str.push_back(std::isalnum(chr) ? chr : '_');
  }
  return &ctx.Idents.get(str);
}

clang::VarDecl *ASTBuilder::CreateVarDecl(clang::DeclContext *decl_ctx,
                                          clang::QualType type,
                                          clang::IdentifierInfo *id,
                                          clang::StorageClass storage_class) {
  return clang::VarDecl::Create(
      ctx, decl_ctx, clang::SourceLocation(), clang::SourceLocation(), id, type,
      ctx.getTrivialTypeSourceInfo(type), storage_class);
}

clang::FunctionDecl *ASTBuilder::CreateFunctionDecl(
    clang::DeclContext *decl_ctx, clang::QualType type,
    clang::IdentifierInfo *id) {
  return clang::FunctionDecl::Create(
      ctx, decl_ctx, clang::SourceLocation(), clang::SourceLocation(),
      clang::DeclarationName(id), type, ctx.getTrivialTypeSourceInfo(type),
      clang::SC_None, /*isInlineSpecified=*/false);
}

clang::ParmVarDecl *ASTBuilder::CreateParamDecl(clang::DeclContext *decl_ctx,
                                                clang::QualType type,
                                                clang::IdentifierInfo *id) {
  return sema.CheckParameter(
      decl_ctx, clang::SourceLocation(), clang::SourceLocation(), id, type,
      ctx.getTrivialTypeSourceInfo(type), clang::SC_None);
}

clang::RecordDecl *ASTBuilder::CreateStructDecl(clang::DeclContext *decl_ctx,
                                                clang::IdentifierInfo *id,
                                                clang::RecordDecl *prev_decl) {
  return clang::RecordDecl::Create(ctx, clang::TagTypeKind::TTK_Struct,
                                   decl_ctx, clang::SourceLocation(),
                                   clang::SourceLocation(), id, prev_decl);
}

clang::RecordDecl *ASTBuilder::CreateUnionDecl(clang::DeclContext *decl_ctx,
                                               clang::IdentifierInfo *id,
                                               clang::RecordDecl *prev_decl) {
  return clang::RecordDecl::Create(ctx, clang::TagTypeKind::TTK_Union, decl_ctx,
                                   clang::SourceLocation(),
                                   clang::SourceLocation(), id, prev_decl);
}

clang::EnumDecl *ASTBuilder::CreateEnumDecl(clang::DeclContext *decl_ctx,
                                            clang::IdentifierInfo *id,
                                            clang::EnumDecl *prev_decl) {
  return clang::EnumDecl::Create(ctx, decl_ctx, clang::SourceLocation(),
                                 clang::SourceLocation(), id, prev_decl, false,
                                 false, false);
}

clang::FieldDecl *ASTBuilder::CreateFieldDecl(clang::RecordDecl *record,
                                              clang::QualType type,
                                              clang::IdentifierInfo *id) {
  return sema.CheckFieldDecl(
      clang::DeclarationName(id), type, ctx.getTrivialTypeSourceInfo(type),
      record, clang::SourceLocation(), /*Mutable=*/false, /*BitWidth=*/nullptr,
      clang::ICIS_NoInit, clang::SourceLocation(),
      clang::AccessSpecifier::AS_none,
      /*PrevDecl=*/nullptr);
}

clang::FieldDecl *ASTBuilder::CreateFieldDecl(clang::RecordDecl *record,
                                              clang::QualType type,
                                              clang::IdentifierInfo *id,
                                              unsigned bitwidth) {
  auto bw{clang::IntegerLiteral::Create(ctx, llvm::APInt(32, bitwidth),
                                        ctx.IntTy, clang::SourceLocation())};
  return sema.CheckFieldDecl(clang::DeclarationName(id), type,
                             ctx.getTrivialTypeSourceInfo(type), record,
                             clang::SourceLocation(), /*Mutable=*/false, bw,
                             clang::ICIS_NoInit, clang::SourceLocation(),
                             clang::AccessSpecifier::AS_none,
                             /*PrevDecl=*/nullptr);
}

clang::EnumConstantDecl *ASTBuilder::CreateEnumConstantDecl(
    clang::EnumDecl *e, clang::IdentifierInfo *id, clang::Expr *expr,
    clang::EnumConstantDecl *previousConstant) {
  return sema.CheckEnumConstant(e, previousConstant, clang::SourceLocation(),
                                id, expr);
}

clang::DeclStmt *ASTBuilder::CreateDeclStmt(clang::Decl *decl) {
  return new (ctx)
      clang::DeclStmt(clang::DeclGroupRef(decl), clang::SourceLocation(),
                      clang::SourceLocation());
}

clang::DeclRefExpr *ASTBuilder::CreateDeclRef(clang::ValueDecl *val) {
  CHECK(val) << "Should not be null in CreateDeclRef.";
  clang::DeclarationNameInfo dni(val->getDeclName(), clang::SourceLocation());
  clang::CXXScopeSpec ss;
  auto er{sema.BuildDeclarationNameExpr(ss, dni, val)};
  CHECK(er.isUsable());
  return er.getAs<clang::DeclRefExpr>();
}

clang::ParenExpr *ASTBuilder::CreateParen(clang::Expr *expr) {
  return new (ctx)
      clang::ParenExpr(clang::SourceLocation(), clang::SourceLocation(), expr);
}

clang::CStyleCastExpr *ASTBuilder::CreateCStyleCast(clang::QualType type,
                                                    clang::Expr *expr) {
  CHECK(expr) << "Should not be null in CreateCStyleCast.";
  if (CExprPrecedence::UnaryOp < GetOperatorPrecedence(expr)) {
    expr = CreateParen(expr);
  }
  auto er{sema.BuildCStyleCastExpr(clang::SourceLocation(),
                                   ctx.getTrivialTypeSourceInfo(type),
                                   clang::SourceLocation(), expr)};
  CHECK(er.isUsable());
  return er.getAs<clang::CStyleCastExpr>();
}

clang::UnaryOperator *ASTBuilder::CreateUnaryOp(clang::UnaryOperatorKind opc,
                                                clang::Expr *expr) {
  CHECK(expr) << "Should not be null in CreateUnaryOp.";
  if (GetOperatorPrecedence(opc) < GetOperatorPrecedence(expr)) {
    expr = CreateParen(expr);
  }
  auto er{sema.CreateBuiltinUnaryOp(clang::SourceLocation(), opc, expr)};
  CHECK(er.isUsable());
  return er.getAs<clang::UnaryOperator>();
}

clang::BinaryOperator *ASTBuilder::CreateBinaryOp(clang::BinaryOperatorKind opc,
                                                  clang::Expr *lhs,
                                                  clang::Expr *rhs) {
  CHECK(lhs && rhs) << "Should not be null in CreateBinaryOp.";
  if (GetOperatorPrecedence(opc) < GetOperatorPrecedence(lhs)) {
    lhs = CreateParen(lhs);
  }
  if (GetOperatorPrecedence(opc) < GetOperatorPrecedence(rhs)) {
    rhs = CreateParen(rhs);
  }
  auto er{sema.CreateBuiltinBinOp(clang::SourceLocation(), opc, lhs, rhs)};
  CHECK(er.isUsable());
  return er.getAs<clang::BinaryOperator>();
}

clang::BinaryOperator *ASTBuilder::CreateBinaryOperator(clang::Expr *lhs,
                                                       clang::Expr *rhs,
                                                       clang::BinaryOperator::Opcode opc,
                                                       clang::QualType res_type) {
  // Convert from BinaryOperator::Opcode to BinaryOperatorKind
  auto kind = static_cast<clang::BinaryOperatorKind>(opc);
  
  // Use the existing CreateBinaryOp implementation which handles
  // parentheses and uses Sema to create the operator with proper type checking
  return CreateBinaryOp(kind, lhs, rhs);
}

clang::ConditionalOperator *ASTBuilder::CreateConditional(clang::Expr *cond,
                                                          clang::Expr *lhs,
                                                          clang::Expr *rhs) {
  auto er{sema.ActOnConditionalOp(clang::SourceLocation(),
                                  clang::SourceLocation(), cond, lhs, rhs)};
  CHECK(er.isUsable());
  return er.getAs<clang::ConditionalOperator>();
}

clang::ArraySubscriptExpr *ASTBuilder::CreateArraySub(clang::Expr *base,
                                                      clang::Expr *idx) {
  CHECK(base && idx) << "Should not be null in CreateArraySub.";
  if (CExprPrecedence::SpecialOp < GetOperatorPrecedence(base)) {
    base = CreateParen(base);
  }
  auto er{sema.CreateBuiltinArraySubscriptExpr(base, clang::SourceLocation(),
                                               idx, clang::SourceLocation())};
  CHECK(er.isUsable());
  return er.getAs<clang::ArraySubscriptExpr>();
}

clang::CallExpr *ASTBuilder::CreateCall(clang::Expr *callee,
                                        std::vector<clang::Expr *> &args) {
  CHECK(callee) << "Should not be null in CreateCall.";
  if (CExprPrecedence::SpecialOp < GetOperatorPrecedence(callee)) {
    callee = CreateParen(callee);
  }
  auto er{sema.BuildCallExpr(/*Scope=*/nullptr, callee, clang::SourceLocation(),
                             args, clang::SourceLocation())};
  CHECK(er.isUsable());
  return er.getAs<clang::CallExpr>();
}

clang::Expr *ASTBuilder::CreateBuiltinCall(clang::Builtin::ID builtin,
                                           std::vector<clang::Expr *> &args) {
  auto name{ctx.BuiltinInfo.getName(builtin)};
  clang::SourceLocation loc;
  clang::LookupResult R(sema, &ctx.Idents.get(name), loc,
                        clang::Sema::LookupOrdinaryName);
  clang::Sema::LookupNameKind NameKind = R.getLookupKind();
  auto II{R.getLookupName().getAsIdentifierInfo()};
  clang::ASTContext::GetBuiltinTypeError error;
  auto ty{ctx.GetBuiltinType(builtin, error)};
  CHECK(!error);
  auto decl{sema.CreateBuiltin(II, ty, builtin, loc)};

  return CreateCall(decl, args);
}

clang::MemberExpr *ASTBuilder::CreateFieldAcc(clang::Expr *base,
                                              clang::FieldDecl *field,
                                              bool is_arrow) {
  CHECK(base && field) << "Should not be null in CreateFieldAcc.";
  CHECK(!is_arrow || base->getType()->isPointerType())
      << "Base operand in arrow operator must be a pointer!";
  clang::CXXScopeSpec ss;
  auto dap{clang::DeclAccessPair::make(field, field->getAccess())};
  auto er{sema.BuildFieldReferenceExpr(base, is_arrow, clang::SourceLocation(),
                                       ss, field, dap,
                                       clang::DeclarationNameInfo())};
  CHECK(er.isUsable());
  return er.getAs<clang::MemberExpr>();
}

clang::InitListExpr *ASTBuilder::CreateInitList(
    std::vector<clang::Expr *> &exprs) {
  auto er{sema.ActOnInitList(clang::SourceLocation(), exprs,
                             clang::SourceLocation())};
  CHECK(er.isUsable());
  return er.getAs<clang::InitListExpr>();
}

clang::CompoundStmt *ASTBuilder::CreateCompoundStmt(
    std::vector<clang::Stmt *> &stmts) {
  // sema.ActOnStartOfCompoundStmt(/*isStmtExpr=*/false);
  // auto sr{sema.ActOnCompoundStmt(clang::SourceLocation(),
  //                                clang::SourceLocation(), stmts,
  //                                /*isStmtExpr=*/false)};
  // sema.ActOnFinishOfCompoundStmt();
  // CHECK(sr.isUsable());
  // return sr.getAs<clang::CompoundStmt>();
  return clang::CompoundStmt::Create(ctx, stmts, clang::FPOptionsOverride{},
                                     clang::SourceLocation(),
                                     clang::SourceLocation());
}

clang::CompoundLiteralExpr *ASTBuilder::CreateCompoundLit(clang::QualType type,
                                                          clang::Expr *expr) {
  auto er{sema.BuildCompoundLiteralExpr(clang::SourceLocation(),
                                        ctx.getTrivialTypeSourceInfo(type),
                                        clang::SourceLocation(), expr)};
  CHECK(er.isUsable());
  return er.getAs<clang::CompoundLiteralExpr>();
}


clang::IfStmt *ASTBuilder::CreateIf(clang::Expr *cond, clang::Stmt *then_val,
                                    clang::Stmt *else_val) {
  CHECK(cond && then_val) << "Should not be null in CreateIf.";
  auto cr{sema.ActOnCondition(/*Scope=*/nullptr, clang::SourceLocation(), cond,
                              clang::Sema::ConditionKind::Boolean)};
  CHECK(!cr.isInvalid());
  auto if_stmt{clang::IfStmt::CreateEmpty(ctx, true, false, false)};
  if_stmt->setCond(cr.get().second);
  if_stmt->setThen(then_val);
  if_stmt->setElse(else_val);
  if_stmt->setStatementKind(clang::IfStatementKind::Ordinary);
  return if_stmt;
}

clang::WhileStmt *ASTBuilder::CreateWhile(clang::Expr *cond,
                                          clang::Stmt *body) {
  // auto sr{sema.ActOnWhileStmt(clang::SourceLocation(),
  // clang::SourceLocation(),
  //                             cond, clang::SourceLocation(), body)};
  // CHECK(sr.isUsable());
  // return sr.getAs<clang::WhileStmt>();
  CHECK(cond != nullptr) << "Should not be null in CreateWhile.";
  auto cer{sema.CheckBooleanCondition(clang::SourceLocation(), cond)};
  CHECK(!cer.isInvalid());
  return clang::WhileStmt::Create(
      ctx, nullptr, cond, body, clang::SourceLocation(),
      clang::SourceLocation(), clang::SourceLocation());
}

clang::DoStmt *ASTBuilder::CreateDo(clang::Expr *cond, clang::Stmt *body) {
  // auto sr{sema.ActOnDoStmt(clang::SourceLocation(), body,
  //                          clang::SourceLocation(),
  //                          clang::SourceLocation(), cond,
  //                          clang::SourceLocation())};
  // CHECK(sr.isUsable());
  // return sr.getAs<clang::DoStmt>();
  CHECK(cond != nullptr) << "Should not be null in CreateDo.";
  auto cer{sema.CheckBooleanCondition(clang::SourceLocation(), cond)};
  CHECK(!cer.isInvalid());
  cer = sema.ActOnFinishFullExpr(cer.get(), clang::SourceLocation(),
                                 /*DiscardedValue=*/false);
  CHECK(!cer.isInvalid());
  return new (ctx)
      clang::DoStmt(body, cond, clang::SourceLocation(),
                    clang::SourceLocation(), clang::SourceLocation());
}

clang::BreakStmt *ASTBuilder::CreateBreak() {
  return new (ctx) clang::BreakStmt(clang::SourceLocation());
}

clang::ReturnStmt *ASTBuilder::CreateReturn(clang::Expr *retval) {
  // auto sr{sema.BuildReturnStmt(clang::SourceLocation(), retval)};
  // CHECK(sr.isUsable());
  // return sr.getAs<clang::ReturnStmt>();
  return clang::ReturnStmt::Create(ctx, clang::SourceLocation(), retval,
                                   nullptr);
}

clang::CXXTryStmt *ASTBuilder::CreateTry(clang::Stmt *try_block,
                                         llvm::ArrayRef<clang::Stmt *> handlers) {
  return clang::CXXTryStmt::Create(ctx, clang::SourceLocation(), try_block,
                                   handlers);
}

clang::CXXCatchStmt *ASTBuilder::CreateCatch(clang::VarDecl *exception_var,
                                             clang::Stmt *handler_block) {
  return new (ctx) clang::CXXCatchStmt(clang::SourceLocation(), exception_var,
                                       handler_block);
}

clang::CXXThrowExpr *ASTBuilder::CreateThrow(clang::Expr *expr) {
  return new (ctx) clang::CXXThrowExpr(
    expr,
    ctx.VoidTy,
    clang::SourceLocation(),
    false  // Does not throw any exceptions
  );
}

clang::CXXTemporaryObjectExpr* ASTBuilder::CreateTemporary(
    clang::QualType type,
    std::vector<clang::Expr*> args) {
  
  // Create constructor declaration
  auto* record = type->getAsCXXRecordDecl();
  if (!record) {
    THROW() << "Type is not a C++ class/struct";
  }
  
  // Find the constructor that matches our arguments
  clang::CXXConstructorDecl* ctor = nullptr;
  for (auto* method : record->ctors()) {
    if (method->getNumParams() == args.size()) {
      ctor = method;
      break;
    }
  }
  
  if (!ctor) {
    THROW() << "No matching constructor found";
  }
  
  // Create the temporary object expression
  auto type_info = ctx.getTrivialTypeSourceInfo(type, clang::SourceLocation());
  
  return clang::CXXTemporaryObjectExpr::Create(
    ctx,
    ctor,
    type,
    type_info,
    args,
    clang::SourceRange(),
    false,  // Had multiple candidates
    false,  // List initialization  
    false,  // Std initializer list
    false   // Zero initialization
  );
}

clang::CXXConstructExpr* ASTBuilder::CreateConstructExpr(
    std::vector<clang::Expr*> args) {
  
  // This function requires a specific type context which we don't have here
  // The caller should use CreateTemporary with a proper type instead
  THROW() << "CreateConstructExpr requires type context - use CreateTemporary instead";
  return nullptr;  // Unreachable, but satisfies compiler
}

clang::TypeSourceInfo* ASTBuilder::CreateTypeRef(std::string type_name) {
  // Create a type reference for a named type
  // This creates an identifier type that will be resolved later
  auto id = CreateIdentifier(type_name);
  
  // For C++ exception types, we need to create proper record types
  clang::QualType type;
  if (type_name.find("std::") == 0) {
    // Create a CXXRecordDecl for the standard library type
    auto record_decl = clang::CXXRecordDecl::Create(
        ctx, clang::TTK_Class, ctx.getTranslationUnitDecl(),
        clang::SourceLocation(), clang::SourceLocation(), id);
    type = ctx.getRecordType(record_decl);
  } else {
    // For other types, create a typedef type
    auto typedef_decl = CreateTypedefDecl(ctx.getTranslationUnitDecl(), id, ctx.IntTy);
    type = ctx.getTypedefType(typedef_decl);
  }
  
  return ctx.getTrivialTypeSourceInfo(type, clang::SourceLocation());
}

clang::TypedefDecl *ASTBuilder::CreateTypedefDecl(clang::DeclContext *decl_ctx,
                                                  clang::IdentifierInfo *id,
                                                  clang::QualType type) {
  return clang::TypedefDecl::Create(ctx, decl_ctx, clang::SourceLocation(),
                                    clang::SourceLocation(), id,
                                    ctx.getTrivialTypeSourceInfo(type));
}

clang::NullStmt *ASTBuilder::CreateNullStmt() {
  return new (ctx) clang::NullStmt(clang::SourceLocation());
}

clang::SwitchStmt *ASTBuilder::CreateSwitchStmt(clang::Expr *cond) {
  auto cc{sema.CheckSwitchCondition(clang::SourceLocation(), cond)};
  CHECK(!cc.isInvalid());
  return clang::SwitchStmt::Create(ctx, nullptr, nullptr, cc.get(),
                                   clang::SourceLocation(),
                                   clang::SourceLocation());
}

clang::CaseStmt *ASTBuilder::CreateCaseStmt(clang::Expr *cond) {
  return clang::CaseStmt::Create(ctx, cond, nullptr, clang::SourceLocation(),
                                 clang::SourceLocation(),
                                 clang::SourceLocation());
}

clang::DefaultStmt *ASTBuilder::CreateDefaultStmt(clang::Stmt *body) {
  return new (ctx) clang::DefaultStmt(clang::SourceLocation(),
                                      clang::SourceLocation(), body);
}

clang::CompoundStmt *ASTBuilder::CreateCommentMarker(const std::string &text) {
  // Create a call to a special marker function that will be recognized during output
  // This creates: __builtin_rellic_bb_marker("bb_name");
  
  // Create the function identifier
  auto id = CreateIdentifier("__builtin_rellic_bb_marker");
  
  // Create a function type: void(const char*)
  clang::QualType char_ptr_type = ctx.getPointerType(ctx.CharTy.withConst());
  clang::QualType func_type = ctx.getFunctionType(
      ctx.VoidTy, {char_ptr_type}, clang::FunctionProtoType::ExtProtoInfo());
  
  // Create a function declaration
  auto func_decl = clang::FunctionDecl::Create(
      ctx, ctx.getTranslationUnitDecl(), clang::SourceLocation(),
      clang::SourceLocation(), id, func_type, nullptr, clang::SC_None);
  func_decl->setImplicit(true);
  
  // Create the string literal argument
  auto str_lit = CreateStrLit(text);
  
  // Create a DeclRefExpr for the function
  auto func_ref = CreateDeclRef(func_decl);
  
  // Create the function call
  auto call = clang::CallExpr::Create(
      ctx, func_ref, {str_lit}, ctx.VoidTy, clang::VK_PRValue,
      clang::SourceLocation(), clang::FPOptionsOverride());
  
  // Wrap in a compound statement
  std::vector<clang::Stmt *> stmts = {call};
  return CreateCompoundStmt(stmts);
}

clang::CXXTryStmt *ASTBuilder::CreateCXXTryStmt(clang::SourceLocation try_loc,
                                               clang::CompoundStmt *try_block,
                                               llvm::ArrayRef<clang::CXXCatchStmt*> handlers) {
  // Convert CXXCatchStmt* to Stmt* for the Create function
  std::vector<clang::Stmt*> stmt_handlers;
  stmt_handlers.reserve(handlers.size());
  for (auto *handler : handlers) {
    stmt_handlers.push_back(handler);
  }
  return clang::CXXTryStmt::Create(ctx, try_loc, try_block, stmt_handlers);
}

clang::CXXCatchStmt *ASTBuilder::CreateCXXCatchStmt(clang::SourceLocation catch_loc,
                                                   clang::VarDecl *exception_decl,
                                                   clang::Stmt *handler_block) {
  return new (ctx) clang::CXXCatchStmt(catch_loc, exception_decl, handler_block);
}

}  // namespace rellic
