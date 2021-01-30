#include "genpybind/expose.h"

#include "genpybind/annotated_decl.h"
#include "genpybind/decl_context_graph.h"
#include "genpybind/decl_context_graph_processing.h"
#include "genpybind/diagnostics.h"
#include "genpybind/string_utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/QualTypeNames.h>
#include <clang/AST/Type.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Sema/Lookup.h>
#include <clang/Sema/Overload.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

#include <cassert>
#include <iterator>
#include <utility>

using namespace genpybind;

namespace {

class DiscriminateIdentifiers {
  llvm::StringMap<unsigned> discriminators;

public:
  std::string discriminate(llvm::StringRef name) {
    auto it = discriminators.try_emplace(name, 0).first;
    const unsigned discriminator = ++it->getValue();
    std::string result{name};
    if (discriminator != 1)
      result += "_" + llvm::utostr(discriminator);
    return result;
  }
};

class IsBeforeInTranslationUnit {
  clang::SourceManager &source_manager;

public:
  explicit IsBeforeInTranslationUnit(clang::SourceManager &source_manager)
      : source_manager(source_manager) {}

  bool operator()(clang::SourceLocation lhs, clang::SourceLocation rhs) const {
    return source_manager.isBeforeInTranslationUnit(lhs, rhs);
  }

  bool operator()(const clang::Decl *lhs, const clang::Decl *rhs) const {
    // TODO: getBeginLoc? Macros?
    return operator()(lhs->getLocation(), rhs->getLocation());
  }
};

/// A very unprincipled attempt to print a default argument expression in
/// a fully qualified way by augmenting and replicating the behavior/output of
/// clang::Stmt::prettyPrint.
/// Where successful, the global namespace specifier (`::`) is prepended to all
/// nested name specifiers.
/// Many useful facilities are not exposed in the clang API, e.g. the helper
/// functions in `QualTypeNames.cpp` and parts of `TreeTransform.h`.  If they
/// are at some point, this hack should be replaced altogether.
struct AttemptFullQualificationPrinter : clang::PrinterHelper {
  const clang::ASTContext &context;
  clang::PrintingPolicy printing_policy;
  bool within_braced_initializer = false;

  AttemptFullQualificationPrinter(const clang::ASTContext &context,
                                  clang::PrintingPolicy printing_policy)
      : context(context), printing_policy(printing_policy) {}

  bool handledStmt(clang::Stmt *stmt, llvm::raw_ostream &os) override {
    const auto *expr = llvm::dyn_cast<clang::Expr>(stmt);
    if (expr == nullptr)
      return false;

    auto printRecursively = [&](const clang::Expr *expr) {
      expr->printPretty(os, this, printing_policy, /*Indentation=*/0,
                        /*NewlineSymbol=*/"\n", &context);
    };

    // Prepend the outermost braced initializer with its fully qualified type.
    bool should_prepend_qualified_type_to_braced_initializer = [&] {
      if (within_braced_initializer)
        return false;
      if (llvm::isa<clang::InitListExpr>(expr))
        return true;
      if (llvm::isa<clang::CXXTemporaryObjectExpr>(expr))
        return false;
      if (const auto *constr = llvm::dyn_cast<clang::CXXConstructExpr>(expr))
        return constr->isListInitialization();
      return false;
    }();

    if (should_prepend_qualified_type_to_braced_initializer) {
      os << clang::TypeName::getFullyQualifiedName(expr->getType(), context,
                                                   printing_policy,
                                                   /*WithGlobalNsPrefix=*/true);
      within_braced_initializer = true;
      printRecursively(expr);
      within_braced_initializer = false;
      return true;
    }

    // Fully qualify all printed types.  Effectively all instances of
    // `getType().print(...)` in `StmtPrinter` have to be re-defined.
    // TODO: It seems the existing implementation mostly does the right thing
    // already, but does not insert global namespace specifiers.  As the
    // bindings live at the global namespace this should not be a problem,
    // though.  Investigate and consider removing this code.
    // TODO: Extend to remaining cases, or replace by more scalable solution.
    // - OffsetOfExpr
    // - CompoundLiteralExpr
    // - ConvertVectorExpr
    // - ImplicitValueInitExpr
    // - VAArgExpr
    // - BuiltinBitCastExpr
    // - CXXTypeidExpr
    // - CXXUuidofExpr
    // - CXXScalarValueInitExpr
    // - CXXUnresolvedConstructExpr
    // - TypeTraitExpr
    // - RequiresExpr
    // - ObjCBridgedCastExpr
    // - BlockExpr
    // - AsTypeExpr

    auto printFullyQualifiedName = [&](clang::QualType qual_type) {
      os << clang::TypeName::getFullyQualifiedName(qual_type, context,
                                                   printing_policy,
                                                   /*WithGlobalNsPrefix=*/true);
    };

    if (const auto *cast = llvm::dyn_cast<clang::CStyleCastExpr>(expr)) {
      os << '(';
      printFullyQualifiedName(cast->getType());
      os << ')';
      printRecursively(cast->getSubExpr());
      return true;
    }

    if (const auto *cast = llvm::dyn_cast<clang::CXXNamedCastExpr>(expr)) {
      os << cast->getCastName() << '<';
      printFullyQualifiedName(cast->getType());
      os << ">(";
      printRecursively(cast->getSubExpr());
      os << ')';
      return true;
    }

    if (const auto *cast = llvm::dyn_cast<clang::CXXFunctionalCastExpr>(expr)) {
      printFullyQualifiedName(cast->getType());
      if (cast->getLParenLoc().isValid())
        os << '(';
      printRecursively(cast->getSubExpr());
      if (cast->getLParenLoc().isValid())
        os << ')';
      return true;
    }

    if (const auto *temporary =
            llvm::dyn_cast<clang::CXXTemporaryObjectExpr>(expr)) {
      printFullyQualifiedName(temporary->getType());

      if (temporary->isStdInitListInitialization())
        /* do nothing; braces are printed by containing expression */;
      else
        os << (temporary->isListInitialization() ? '{' : '(');

      bool comma = false;
      for (const clang::Expr *argument : temporary->arguments()) {
        if (argument->isDefaultArgument())
          break;
        if (comma)
          os << ", ";
        printRecursively(argument);
        comma = true;
      }

      if (temporary->isStdInitListInitialization())
        /* do nothing */;
      else
        os << (temporary->isListInitialization() ? '}' : ')');

      return true;
    }

    // Fully qualify nested name specifiers.  This corresponds to calls to
    // `getQualifier()` in `StmtPrinter`, which have to be re-defined.

    if (const auto *decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
      os << "::";
      decl_ref->getFoundDecl()->printNestedNameSpecifier(os, printing_policy);
      if (decl_ref->hasTemplateKeyword())
        os << "template ";
      os << decl_ref->getNameInfo();
      if (decl_ref->hasExplicitTemplateArgs())
        clang::printTemplateArgumentList(os, decl_ref->template_arguments(),
                                         printing_policy);
      return true;
    }

    if (const auto *member = llvm::dyn_cast<clang::MemberExpr>(expr)) {
      assert(!printing_policy.SuppressImplicitBase &&
             "suppressing implicit 'this' not implemented");
      const clang::Expr *base = member->getBase();
      printRecursively(base);

      bool suppress_member_access_operator = [&] {
        if (const auto *parent_member = llvm::dyn_cast<clang::MemberExpr>(base))
          if (const auto *field = llvm::dyn_cast<clang::FieldDecl>(
                  parent_member->getMemberDecl()))
            return field->isAnonymousStructOrUnion();
        return false;
      }();

      if (!suppress_member_access_operator)
        os << (member->isArrow() ? "->" : ".");

      os << "::";
      member->getFoundDecl()->printNestedNameSpecifier(os, printing_policy);
      if (member->hasTemplateKeyword())
        os << "template ";
      os << member->getMemberNameInfo();
      if (member->hasExplicitTemplateArgs())
        clang::printTemplateArgumentList(os, member->template_arguments(),
                                         printing_policy);
      return true;
    }

    return false;
  }
};

} // namespace

static void emitStringLiteral(llvm::raw_ostream &os, llvm::StringRef text) {
  os << '"';
  os.write_escaped(text);
  os << '"';
}

static llvm::StringRef getBriefText(const clang::Decl *decl) {
  const clang::ASTContext &context = decl->getASTContext();
  if (const clang::RawComment *raw = context.getRawCommentForAnyRedecl(decl)) {
    return raw->getBriefText(context);
  }
  return {};
}

static llvm::StringRef getDocstring(const clang::FunctionDecl *function) {
  llvm::StringRef result = getBriefText(function);
  if (result.empty())
    if (const clang::FunctionTemplateDecl *primary =
            function->getPrimaryTemplate())
      result = getBriefText(primary);
  return result;
}

static clang::PrintingPolicy
getPrintingPolicyForExposedNames(const clang::ASTContext &context) {
  auto policy = context.getPrintingPolicy();
  policy.SuppressScope = false;
  policy.AnonymousTagLocations = false;
  policy.PolishForDeclaration = true;
  return policy;
}

static void emitParameterTypes(llvm::raw_ostream &os,
                               const clang::FunctionDecl *function) {
  const clang::ASTContext &context = function->getASTContext();
  auto policy = getPrintingPolicyForExposedNames(context);
  bool comma = false;
  for (const clang::ParmVarDecl *param : function->parameters()) {
    if (comma)
      os << ", ";
    os << clang::TypeName::getFullyQualifiedName(param->getOriginalType(),
                                                 context, policy,
                                                 /*WithGlobalNsPrefix=*/true);
    comma = true;
  }
}

static void emitParameters(llvm::raw_ostream &os,
                           const AnnotatedFunctionDecl *annotation) {
  const auto *function = llvm::cast<clang::FunctionDecl>(annotation->getDecl());
  const clang::ASTContext &context = function->getASTContext();
  auto printing_policy = getPrintingPolicyForExposedNames(context);
  AttemptFullQualificationPrinter printer_helper{context, printing_policy};
  unsigned index = 0;
  for (const clang::ParmVarDecl *param : function->parameters()) {
    // TODO: Do not emit `arg()` for `pybind11::{kw,}args`.
    os << ", ::pybind11::arg(";
    emitStringLiteral(os, param->getName());
    os << ")";
    if (annotation->noconvert.count(index) != 0)
      os << ".noconvert()";
    if (annotation->required.count(index) != 0)
      os << ".none(false)";
    if (const clang::Expr *expr = param->getDefaultArg()) {
      // os << "/* = ";
      os << " = ";
      expr->printPretty(os, &printer_helper, printing_policy, /*Indentation=*/0,
                        /*NewlineSymbol=*/"\n", &context);
      // os << " */";
    }
    ++index;
  }
}

static void emitPolicies(llvm::raw_ostream &os,
                         const AnnotatedFunctionDecl *annotation) {
  if (!annotation->return_value_policy.empty())
    os << ", pybind11::return_value_policy::"
       << annotation->return_value_policy;
  for (const auto &item : annotation->keep_alive) {
    os << ", pybind11::keep_alive<" << item.first << ", " << item.second
       << ">()";
  }
}

static void emitFunctionPointer(llvm::raw_ostream &os,
                                const clang::FunctionDecl *function) {
  // TODO: All names need to be printed in a fully-qualified way (also nested
  // template arguments)
  auto policy = getPrintingPolicyForExposedNames(function->getASTContext());
  os << "::pybind11::overload_cast<";
  emitParameterTypes(os, function);
  os << ">(&::";
  function->printQualifiedName(os, policy);
  if (const clang::TemplateArgumentList *args =
          function->getTemplateSpecializationArgs()) {
    clang::printTemplateArgumentList(os, args->asArray(), policy);
  }
  if (function->getType()->castAs<clang::FunctionType>()->isConst())
    os << ", ::pybind11::const_";
  os << ")";
}

static void emitManualBindings(llvm::raw_ostream &os,
                               const clang::ASTContext &context,
                               const clang::LambdaExpr *manual_bindings) {
  assert(manual_bindings != nullptr);
  auto printing_policy = getPrintingPolicyForExposedNames(context);
  // Print and immediately invoke manual bindings lambda(IIFE).
  // TODO: This requires all referenced types and declarations in the manual
  // binding code to be fully qualified.  It would be useful to atleast warn if
  // this is not the case.  Unfortunately, automatically qualifying these
  // references is not straight forward.  See `AttemptFullQualificationPrinter`,
  // which could be a starting point except for the fact that it has no
  // influence on the printing of decls within the lambda expression.
  const clang::CXXMethodDecl *method = manual_bindings->getCallOperator();
  assert(method->getNumParams() == 1);
  const clang::ParmVarDecl *param = method->getParamDecl(0);
  bool needs_context = param->isUsed() || param->isReferenced();
  if (needs_context)
    os << "[](auto &" << param->getName() << ") ";
  manual_bindings->getBody()->printPretty(os, nullptr, printing_policy,
                                          /*Indentation=*/0,
                                          /*NewlineSymbol=*/"\n", &context);
  if (needs_context)
    os << "(context);\n";
}

static std::vector<const clang::NamedDecl *>
collectOperatorDeclsViaArgumentDependentLookup(
    clang::Sema &sema, const clang::CXXRecordDecl *record) {
  std::vector<const clang::NamedDecl *> result;
  const clang::ASTContext &ast_context = sema.getASTContext();
  clang::SourceLocation op_loc;

  // Use argument dependent lookup to find operators in a record's associated
  // namespace.  To this end, a fake argument list with an emulated
  // declval<T>() expression is used.  Inside `ArgumentDependentLookup` the
  // arguments are only used for this purpose and are not checked against the
  // function signatures, so a single entry is sufficient.
  const clang::QualType record_type = ast_context.getTypeDeclType(record);
  assert(record_type->isObjectType());
  clang::QualType expr_type = ast_context.getRValueReferenceType(record_type)
                                  .getNonLValueExprType(ast_context);
  clang::OpaqueValueExpr declval_expr(
      op_loc, expr_type, clang::Expr::getValueKindForType(expr_type));
  clang::Expr *args_for_adl[1] = {&declval_expr};

  // At least one parameter has to accept the record type, without
  // implicit conversions.
  auto has_type_of_record = [&](const clang::ParmVarDecl *param) -> bool {
    clang::QualType param_type = param->getType();
    if (param_type->isReferenceType())
      param_type = param_type->getPointeeType();
    return ast_context.hasSameUnqualifiedType(param_type, record_type);
  };
  auto handle_decl = [&](clang::NamedDecl *decl) {
    const auto *function = llvm::cast<clang::FunctionDecl>(decl);
    if (llvm::any_of(function->parameters(), has_type_of_record))
      result.push_back(decl);
  };

  clang::ADLResult operator_decls;
  for (clang::OverloadedOperatorKind op_kind : {
           clang::OO_Plus,
           clang::OO_Minus,
           clang::OO_Star,
           clang::OO_Slash,
           clang::OO_Percent,
           clang::OO_Caret,
           clang::OO_Amp,
           clang::OO_Pipe,
           clang::OO_Tilde,
           clang::OO_Less,
           clang::OO_Greater,
           clang::OO_PlusEqual,
           clang::OO_MinusEqual,
           clang::OO_StarEqual,
           clang::OO_SlashEqual,
           clang::OO_PercentEqual,
           clang::OO_CaretEqual,
           clang::OO_AmpEqual,
           clang::OO_PipeEqual,
           clang::OO_LessLess,
           clang::OO_GreaterGreater,
           clang::OO_LessLessEqual,
           clang::OO_GreaterGreaterEqual,
           clang::OO_EqualEqual,
           clang::OO_ExclaimEqual,
           clang::OO_LessEqual,
           clang::OO_GreaterEqual,
           clang::OO_Spaceship,
       }) {
    // Rewritten candidates are not considered here; they are taken into
    // account when deciding what operators to emit in `RecordExposer`.
    clang::DeclarationName op_name =
        ast_context.DeclarationNames.getCXXOperatorName(op_kind);
    sema.ArgumentDependentLookup(op_name, op_loc, args_for_adl, operator_decls);
  }

  for (clang::NamedDecl *decl : operator_decls) {
    if (const auto *tpl = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
      llvm::for_each(tpl->specializations(), handle_decl);
    } else {
      handle_decl(decl);
    }
  }

  return result;
}

static llvm::StringRef
pythonUnaryOperatorName(clang::OverloadedOperatorKind kind) {
  switch (kind) {
  case clang::OO_Plus:
    return "__pos__";
  case clang::OO_Minus:
    return "__neg__";
  case clang::OO_Tilde:
    return "__invert__";
  default:
    return {};
  }
}

static llvm::StringRef
pythonBinaryOperatorName(clang::OverloadedOperatorKind kind, bool reverse) {
  if (reverse) {
    switch (kind) {
    case clang::OO_Plus:
      return "__radd__";
    case clang::OO_Minus:
      return "__rsub__";
    case clang::OO_Star:
      return "__rmul__";
    case clang::OO_Slash:
      return "__rtruediv__";
    case clang::OO_Percent:
      return "__rmod__";
    case clang::OO_Caret:
      return "__rxor__";
    case clang::OO_Amp:
      return "__rand__";
    case clang::OO_Pipe:
      return "__ror__";
    case clang::OO_Less:
      return "__gt__";
    case clang::OO_Greater:
      return "__lt__";
    case clang::OO_LessLess:
      return "__rlshift__";
    case clang::OO_GreaterGreater:
      return "__rrshift__";
    case clang::OO_EqualEqual:
      return "__eq__";
    case clang::OO_ExclaimEqual:
      return "__ne__";
    case clang::OO_LessEqual:
      return "__ge__";
    case clang::OO_GreaterEqual:
      return "__le__";
    default:
      return {};
    }
  }
  switch (kind) {
  case clang::OO_Plus:
    return "__add__";
  case clang::OO_Minus:
    return "__sub__";
  case clang::OO_Star:
    return "__mul__";
  case clang::OO_Slash:
    return "__truediv__";
  case clang::OO_Percent:
    return "__mod__";
  case clang::OO_Caret:
    return "__xor__";
  case clang::OO_Amp:
    return "__and__";
  case clang::OO_Pipe:
    return "__or__";
  case clang::OO_Less:
    return "__lt__";
  case clang::OO_Greater:
    return "__gt__";
  case clang::OO_PlusEqual:
    return "__iadd__";
  case clang::OO_MinusEqual:
    return "__isub__";
  case clang::OO_StarEqual:
    return "__imul__";
  case clang::OO_SlashEqual:
    return "__itruediv__";
  case clang::OO_PercentEqual:
    return "__imod__";
  case clang::OO_CaretEqual:
    return "__ixor__";
  case clang::OO_AmpEqual:
    return "__iand__";
  case clang::OO_PipeEqual:
    return "__ior__";
  case clang::OO_LessLess:
    return "__lshift__";
  case clang::OO_GreaterGreater:
    return "__rshift__";
  case clang::OO_LessLessEqual:
    return "__ilshift__";
  case clang::OO_GreaterGreaterEqual:
    return "__irshift__";
  case clang::OO_EqualEqual:
    return "__eq__";
  case clang::OO_ExclaimEqual:
    return "__ne__";
  case clang::OO_LessEqual:
    return "__le__";
  case clang::OO_GreaterEqual:
    return "__ge__";
  default:
    return {};
  }
}

std::string genpybind::getFullyQualifiedName(const clang::TypeDecl *decl) {
  const clang::ASTContext &context = decl->getASTContext();
  const clang::QualType qual_type = context.getTypeDeclType(decl);
  auto policy = getPrintingPolicyForExposedNames(context);
  return clang::TypeName::getFullyQualifiedName(qual_type, context, policy,
                                                /*WithGlobalNsPrefix=*/true);
}

TranslationUnitExposer::TranslationUnitExposer(
    clang::Sema &sema, const DeclContextGraph &graph,
    const EffectiveVisibilityMap &visibilities, AnnotationStorage &annotations)
    : sema(sema), graph(graph), visibilities(visibilities),
      annotations(annotations) {}

void TranslationUnitExposer::emitModule(llvm::raw_ostream &os,
                                        llvm::StringRef name) {
  const EnclosingScopeMap parents = findEnclosingScopes(graph, annotations);

  const clang::DeclContext *cycle = nullptr;
  const auto sorted_contexts =
      declContextsSortedByDependencies(graph, parents, &cycle);
  if (cycle != nullptr) {
    // TODO: Report this before any other output, ideally pointing to the
    // typedef name decl for `expose_here` cycles.
    Diagnostics::report(llvm::cast<clang::Decl>(cycle),
                        Diagnostics::Kind::ExposeHereCycleError);
    return;
  }

  llvm::DenseMap<const clang::DeclContext *, std::string> context_identifiers(
      static_cast<unsigned>(sorted_contexts.size() + 1));
  // `nullptr` is used in the enclosing scope map to indicate the absence of an
  // enclosing scope (which should only happen for the TU / root node).
  // Consequently, treat `nullptr` as the module root.
  context_identifiers[nullptr] = "root";

  struct WorklistItem {
    const clang::DeclContext *decl_context;
    std::unique_ptr<DeclContextExposer> exposer;
    llvm::StringRef identifier;
  };

  std::vector<WorklistItem> worklist;
  worklist.reserve(sorted_contexts.size());

  {
    llvm::DenseMap<const clang::NamespaceDecl *, const clang::DeclContext *>
        covered_namespaces;
    DiscriminateIdentifiers used_identifiers;
    for (const clang::DeclContext *decl_context : sorted_contexts) {
      // Since the decls to expose in each context are discovered via the name
      // lookup mechanism below, it's sufficient to visit every loookup context
      // once, instead of e.g. visiting each re-opened namespace.
      if (const auto *ns = llvm::dyn_cast<clang::NamespaceDecl>(decl_context)) {
        auto inserted = covered_namespaces.try_emplace(
            ns->getOriginalNamespace(), decl_context);
        if (!inserted.second) {
          auto existing_identifier =
              context_identifiers.find(inserted.first->getSecond());
          assert(existing_identifier != context_identifiers.end() &&
                 "identifier should have been stored at this point");
          auto result = context_identifiers.try_emplace(
              decl_context, existing_identifier->getSecond());
          assert(result.second);
          continue;
        }
      }

      llvm::SmallString<128> name("context");
      if (auto const *type_decl =
              llvm::dyn_cast<clang::TypeDecl>(decl_context)) {
        name += getFullyQualifiedName(type_decl);
      } else if (auto const *ns_decl =
                     llvm::dyn_cast<clang::NamedDecl>(decl_context)) {
        name.push_back('_');
        name += ns_decl->getQualifiedNameAsString();
      }
      makeValidIdentifier(name);
      auto result = context_identifiers.try_emplace(
          decl_context, used_identifiers.discriminate(name));
      assert(result.second);
      llvm::StringRef identifier = result.first->getSecond();
      worklist.push_back(
          {decl_context,
           DeclContextExposer::create(graph, annotations, decl_context),
           identifier});
    }
  }

  // Emit declarations for `expose_` functions
  for (const auto &item : worklist) {
    os << "void expose_" << item.identifier << "(";
    item.exposer->emitParameter(os);
    os << ");\n";
  }

  os << '\n';

  // Emit module definition
  os << "PYBIND11_MODULE(" << name << ", root) {\n";

  // Emit context introducers
  for (const auto &item : worklist) {
    const clang::DeclContext *parent = parents.lookup(item.decl_context);
    assert((parent != nullptr) ^
               llvm::isa<clang::TranslationUnitDecl>(item.decl_context) &&
           "(only) non-TU contexts should have a parent scope");
    auto parent_identifier = context_identifiers.find(parent);
    assert(parent_identifier != context_identifiers.end() &&
           "identifier should have been stored at this point");

    os << "auto " << item.identifier << " = ";
    item.exposer->emitIntroducer(os, parent_identifier->getSecond());
    os << ";\n";
  }

  os << '\n';

  // Emit calls to `expose_` functions
  for (const auto &item : worklist) {
    os << "expose_" << item.identifier << "(" << item.identifier << ");\n";
  }

  { // Emit 'postamble' manual bindings.
    const clang::DeclContext *decl_context = graph.getRoot()->getDeclContext();
    for (clang::DeclContext::specific_decl_iterator<clang::VarDecl>
             it(decl_context->decls_begin()),
         end_it(decl_context->decls_end());
         it != end_it; ++it) {
      const auto *annotation =
          llvm::cast<AnnotatedFieldOrVarDecl>(annotations.getOrInsert(*it));
      if (!annotation->postamble || annotation->manual_bindings == nullptr)
        continue;
      const clang::ASTContext &ast_context = it->getASTContext();
      os << "\n";
      emitManualBindings(os, ast_context, annotation->manual_bindings);
    }
  }

  os << "}\n\n";

  // Emit definitions for `expose_` functions
  // (can be partitioned into several files in the future)
  for (const auto &item : worklist) {
    os << "void expose_" << item.identifier << "(";
    item.exposer->emitParameter(os);
    os << ") {\n";

    // For inlined decls use the default visibility of the current
    // lookup context.
    bool default_visibility = [&] {
      auto it = visibilities.find(item.decl_context);
      return it != visibilities.end() ? it->getSecond() : false;
    }();

    auto handle_decl = [&](const clang::NamedDecl *proposed_decl) {
      const auto *annotation = llvm::dyn_cast<AnnotatedNamedDecl>(
          annotations.getOrInsert(proposed_decl));
      item.exposer->handleDecl(os, proposed_decl, annotation,
                               default_visibility);
    };

    std::vector<const clang::NamedDecl *> decls =
        collectVisibleDeclsFromDeclContext(sema, item.decl_context,
                                           item.exposer->inliningPolicy());
    llvm::sort(decls, IsBeforeInTranslationUnit(sema.getSourceManager()));

    // Inject operators from a record's associated namespace (found via ADL),
    // as these need to be exposed as methods of the record.  Only user-defined
    // operators that can be called without conversions are considered.
    if (const auto *record =
            llvm::dyn_cast<clang::CXXRecordDecl>(item.decl_context)) {
      std::vector<const clang::NamedDecl *> associated_decls =
          collectOperatorDeclsViaArgumentDependentLookup(sema, record);
      llvm::copy(associated_decls, std::back_inserter(decls));
    }

    for (const clang::NamedDecl *proposed_decl : decls) {
      // If there are several declarations of a function template,
      // only one is picked up here.  Thus all specializations can be
      // processed unconditionally.
      if (const auto *tpl =
              llvm::dyn_cast<clang::FunctionTemplateDecl>(proposed_decl)) {
        llvm::for_each(tpl->specializations(), handle_decl);
      } else {
        handle_decl(proposed_decl);
      }
    }

    item.exposer->finalizeDefinition(os);
    os << "}\n\n";
  }
}

std::unique_ptr<DeclContextExposer>
DeclContextExposer::create(const DeclContextGraph &graph,
                           AnnotationStorage &annotations,
                           const clang::DeclContext *decl_context) {
  assert(decl_context != nullptr);
  const auto *decl = llvm::cast<clang::Decl>(decl_context);
  if (const AnnotatedDecl *annotated_decl = annotations.getOrInsert(decl)) {
    if (const auto *ad = llvm::dyn_cast<AnnotatedNamespaceDecl>(annotated_decl))
      return std::make_unique<NamespaceExposer>(ad);
    if (const auto *ad = llvm::dyn_cast<AnnotatedEnumDecl>(annotated_decl))
      return std::make_unique<EnumExposer>(ad);
    if (const auto *ad = llvm::dyn_cast<AnnotatedRecordDecl>(annotated_decl))
      return std::make_unique<RecordExposer>(graph, ad);
  }
  if (DeclContextGraph::accepts(decl))
    return std::make_unique<DeclContextExposer>();

  llvm_unreachable("Unknown declaration context kind.");
}

llvm::Optional<RecordInliningPolicy>
DeclContextExposer::inliningPolicy() const {
  return llvm::None;
}

void DeclContextExposer::emitParameter(llvm::raw_ostream &os) {
  os << "::pybind11::module& context";
}

void DeclContextExposer::emitIntroducer(llvm::raw_ostream &os,
                                        llvm::StringRef parent_identifier) {
  os << parent_identifier;
}

void DeclContextExposer::handleDecl(llvm::raw_ostream &os,
                                    const clang::NamedDecl *decl,
                                    const AnnotatedNamedDecl *annotation,
                                    bool default_visibility) {
  assert(decl != nullptr);
  assert(annotation != nullptr);
  if (!annotation->visible.getValueOr(default_visibility))
    return;

  return handleDeclImpl(os, decl, annotation);
}

void DeclContextExposer::handleDeclImpl(llvm::raw_ostream &os,
                                        const clang::NamedDecl *decl,
                                        const AnnotatedNamedDecl *annotation) {
  assert(!llvm::isa<AnnotatedConstructorDecl>(annotation) &&
         "constructors are handled in RecordExposer");
  const clang::ASTContext &ast_context = decl->getASTContext();
  auto printing_policy = getPrintingPolicyForExposedNames(ast_context);

  if (const auto *annot =
          llvm::dyn_cast<AnnotatedTypedefNameDecl>(annotation)) {
    // Type aliases are hidden by default and do not inherit the default
    // visibility, thus a second check is necessary here.
    if (!annot->visible.hasValue())
      return;
    if (annot->expose_here)
      return;

    const auto *alias = llvm::cast<clang::TypedefNameDecl>(decl);
    const clang::TagDecl *target = alias->getUnderlyingType()->getAsTagDecl();
    if (target == nullptr)
      return;

    os << "context.attr(";
    emitStringLiteral(os, annot->getSpelling());
    os << ") = ::genpybind::getObjectForType<" << getFullyQualifiedName(target)
       << ">();\n";

    return;
  }

  if (const auto *annot = llvm::dyn_cast<AnnotatedFieldOrVarDecl>(annotation)) {
    if (annot->manual_bindings != nullptr) {
      if (!annot->postamble)
        emitManualBindings(os, ast_context, annot->manual_bindings);
      return;
    }
    // For fields and static member variables see `RecordExposer`.
    assert(!llvm::isa<clang::FieldDecl>(decl) &&
           "should have been processed by RecordExposer");
    os << "context.attr(";
    emitStringLiteral(os, annot->getSpelling());
    os << ") = ::";
    decl->printQualifiedName(os, printing_policy);
    os << ";\n";
    return;
  }

  if (const auto *annot = llvm::dyn_cast<AnnotatedMethodDecl>(annotation)) {
    // Properties are handled separately in `RecordExposer::finalizeDefinition`.
    if (!annot->getter_for.empty() || !annot->setter_for.empty())
      return;
  }

  if (const auto *annot = llvm::dyn_cast<AnnotatedFunctionDecl>(annotation)) {
    const auto *function = llvm::cast<clang::FunctionDecl>(decl);
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(decl);

    if (function->isDeleted())
      return;

    bool is_call_operator = function->getOverloadedOperator() == clang::OO_Call;

    // Both operators defined as member functions and operators in a record's
    // associated namespace are handled by `RecordExposer`, thus most operators
    // are ignored here.
    if (function->isOverloadedOperator() && !is_call_operator)
      return;

    std::string spelling = (!is_call_operator || !annot->spelling.empty())
                               ? annot->getSpelling()
                               : "__call__";

    os << ((method != nullptr && method->isStatic()) ? "context.def_static("
                                                     : "context.def(");
    emitStringLiteral(os, spelling);
    os << ", ";
    emitFunctionPointer(os, function);
    os << ", ";
    emitStringLiteral(os, getDocstring(function));
    emitParameters(os, annot);
    emitPolicies(os, annot);
    os << ");\n";
  }
}

void DeclContextExposer::finalizeDefinition(llvm::raw_ostream &os) {
  os << "(void)context;\n";
}

NamespaceExposer::NamespaceExposer(const AnnotatedNamespaceDecl *annotated_decl)
    : annotated_decl(annotated_decl) {}

void NamespaceExposer::emitIntroducer(llvm::raw_ostream &os,
                                      llvm::StringRef parent_identifier) {
  os << parent_identifier;
  if (annotated_decl != nullptr && annotated_decl->module) {
    os << ".def_submodule(";
    emitStringLiteral(os, annotated_decl->getSpelling());
    os << ")";
  }
}

EnumExposer::EnumExposer(const AnnotatedEnumDecl *annotated_decl)
    : annotated_decl(annotated_decl) {
  assert(annotated_decl != nullptr);
}

void EnumExposer::emitParameter(llvm::raw_ostream &os) {
  emitType(os);
  os << "& context";
}

void EnumExposer::emitIntroducer(llvm::raw_ostream &os,
                                 llvm::StringRef parent_identifier) {
  emitType(os);
  os << "(" << parent_identifier << ", ";
  emitStringLiteral(os, annotated_decl->getSpelling());
  if (annotated_decl->arithmetic)
    os << ", ::pybind11::arithmetic()";
  os << ")";
}

void EnumExposer::finalizeDefinition(llvm::raw_ostream &os) {
  const auto *decl = llvm::cast<clang::EnumDecl>(annotated_decl->getDecl());
  if (annotated_decl->export_values.getValueOr(!decl->isScoped()))
    os << "context.export_values();\n";
}

void EnumExposer::emitType(llvm::raw_ostream &os) {
  os << "::pybind11::enum_<"
     << getFullyQualifiedName(
            llvm::cast<clang::TypeDecl>(annotated_decl->getDecl()))
     << ">";
}

void EnumExposer::handleDeclImpl(llvm::raw_ostream &os,
                                 const clang::NamedDecl *decl,
                                 const AnnotatedNamedDecl *annotation) {
  if (const auto *enumerator = llvm::dyn_cast<clang::EnumConstantDecl>(decl)) {
    const auto *enum_decl =
        llvm::cast<clang::EnumDecl>(annotated_decl->getDecl());
    const std::string scope = getFullyQualifiedName(enum_decl);
    os << "context.value(";
    emitStringLiteral(os, annotation->getSpelling());
    os << ", " << scope << "::" << enumerator->getName() << ");\n";
  }
}

RecordExposer::RecordExposer(const DeclContextGraph &graph,
                             const AnnotatedRecordDecl *annotated_decl)
    : graph(graph), annotated_decl(annotated_decl) {
  assert(annotated_decl != nullptr);
}

llvm::Optional<RecordInliningPolicy> RecordExposer::inliningPolicy() const {
  return RecordInliningPolicy::createFromAnnotation(*annotated_decl);
}

void RecordExposer::emitParameter(llvm::raw_ostream &os) {
  emitType(os);
  os << "& context";
}

void RecordExposer::emitIntroducer(llvm::raw_ostream &os,
                                   llvm::StringRef parent_identifier) {
  emitType(os);
  os << "(" << parent_identifier << ", ";
  emitStringLiteral(os, annotated_decl->getSpelling());
  if (annotated_decl->dynamic_attr)
    os << ", ::pybind11::dynamic_attr()";
  os << ")";
}

void RecordExposer::finalizeDefinition(llvm::raw_ostream &os) {
  emitProperties(os);
  os << "context.doc() = ";
  emitStringLiteral(os, getBriefText(annotated_decl->getDecl()));
  os << ";\n";
}

void RecordExposer::emitProperties(llvm::raw_ostream &os) {
  // TODO: Sort / make deterministic
  for (const auto &entry : properties) {
    llvm::StringRef name = entry.getKey();
    const Property &property = entry.getValue();
    if (property.getter == nullptr) {
      if (property.setter == nullptr)
        continue;
      Diagnostics::report(property.setter,
                          Diagnostics::Kind::PropertyHasNoGetterError)
          << name;
      continue;
    }
    bool writable = property.setter != nullptr;
    os << (writable ? "context.def_property("
                    : "context.def_property_readonly(");
    emitStringLiteral(os, name);
    os << ", ";
    emitFunctionPointer(os, property.getter);
    if (writable) {
      os << ", ";
      emitFunctionPointer(os, property.setter);
    }
    os << ");\n";
  }
}

void RecordExposer::emitOperator(llvm::raw_ostream &os,
                                 const clang::FunctionDecl *function) {
  const auto *record_decl =
      llvm::cast<clang::CXXRecordDecl>(annotated_decl->getDecl());
  const clang::ASTContext &ast_context = record_decl->getASTContext();
  const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function);

  clang::OverloadedOperatorKind kind = function->getOverloadedOperator();

  switch (kind) {
  default:
    break;
  case clang::OO_New:
  case clang::OO_Delete:
  case clang::OO_Array_New:
  case clang::OO_Array_Delete:
  case clang::OO_Equal:
  case clang::OO_Spaceship: // TODO: not implemented yet
  case clang::OO_AmpAmp:
  case clang::OO_PipePipe:
  case clang::OO_PlusPlus:
  case clang::OO_MinusMinus:
  case clang::OO_Comma:
  case clang::OO_ArrowStar:
  case clang::OO_Arrow:
  case clang::OO_Call:
  case clang::OO_Subscript:
    return;
  }

  // For each operator, emit all viable rewritten candidates.
  // Note that this would expose e.g. `__eq__` twice for records that have
  // both `==` and `<=>`, but exposing the same operator multiple times
  // should be benign: pybind11 will just pick one definition when it
  // resolves the call.
  bool allow_rewritten_candidates = ast_context.getLangOpts().CPlusPlus2a;
  // TODO: implement this...
  (void)allow_rewritten_candidates;

  llvm::SmallVector<clang::QualType, 2> parameter_types;
  clang::QualType record_type = ast_context.getTypeDeclType(record_decl);
  if (method != nullptr) {
    // TODO: Consider ref qualifiers.
    if (method->isConst())
      record_type = record_type.withConst();
    parameter_types.push_back(ast_context.getLValueReferenceType(record_type));
  }
  for (const clang::ParmVarDecl *param : function->parameters()) {
    parameter_types.push_back(param->getType());
  }

  bool unary = parameter_types.size() == 1;

  // Do not expose address-of and indirection/dereference operators.
  if (unary && (kind == clang::OO_Star || kind == clang::OO_Amp))
    return;

  // Reversing the parameters can also lead to duplicate definitions of
  // relational operators.  E.g., `operator<(int, T)` and `operator>(T, int)`
  // are both exposed as `T.__gt__`.
  // This is considered acceptable for the same reasons as stated above.
  // TODO: This might lead to confusion if the different underlying operators
  // are not compatible (e.g., due to a bug).  Reconsider the trade-off.
  bool reverse_parameters = [&] {
    clang::QualType lhs_param_type = parameter_types.front();
    if (lhs_param_type->isReferenceType())
      lhs_param_type = lhs_param_type->getPointeeType();
    return !ast_context.hasSameUnqualifiedType(lhs_param_type, record_type);
  }();
  os << "context.def(";
  emitStringLiteral(os,
                    unary ? pythonUnaryOperatorName(kind)
                          : pythonBinaryOperatorName(kind, reverse_parameters));
  os << ", ";
  emitOperatorDefinition(os, ast_context, kind, parameter_types,
                         function->getReturnType(), reverse_parameters);
  os << ", ";
  // TODO: Add support for return value policies, if supported by pybind11.
  emitStringLiteral(os, getDocstring(function));
  os << ", ::pybind11::is_operator());\n";
}

void RecordExposer::emitOperatorDefinition(
    llvm::raw_ostream &os, const clang::ASTContext &ast_context,
    clang::OverloadedOperatorKind kind,
    const llvm::SmallVectorImpl<clang::QualType> &parameter_types,
    clang::QualType return_type,
    bool reverse_parameters) {
  assert(parameter_types.size() <= 2);
  bool unary = parameter_types.size() == 1;
  llvm::StringRef parameter_names[2] = {"lhs", "rhs"};
  auto printing_policy = getPrintingPolicyForExposedNames(ast_context);
  os << "[](";
  bool comma = false;
  int parameter_count = static_cast<int>(parameter_types.size());
  for (int index : llvm::seq(0, parameter_count)) {
    if (reverse_parameters)
      index = parameter_count - 1 - index;
    if (comma)
      os << ", ";
    // TODO: If the operator decl takes a parameter by value, this wrapper
    // does so, too.  This might not always work or be optimal?
    os << clang::TypeName::getFullyQualifiedName(
        parameter_types[static_cast<std::size_t>(index)], ast_context,
        printing_policy,
        /*WithGlobalNsPrefix=*/true);
    os << ' ' << parameter_names[index];
    comma = true;
  }
  os << ") -> "
     << clang::TypeName::getFullyQualifiedName(return_type, ast_context,
                                               printing_policy,
                                               /*WithGlobalNsPrefix=*/true)
     << " { return ";
  if (unary) {
    os << getOperatorSpelling(kind) << parameter_names[0];
  } else {
    os << parameter_names[0] << ' ' << getOperatorSpelling(kind) << ' '
       << parameter_names[1];
  }
  os << "; }";
}

void RecordExposer::emitType(llvm::raw_ostream &os) {
  os << "::pybind11::class_<"
     << getFullyQualifiedName(
            llvm::cast<clang::TypeDecl>(annotated_decl->getDecl()));

  // Add all exposed (i.e. part of graph) public bases as arguments.
  if (const auto *decl =
          llvm::dyn_cast<clang::CXXRecordDecl>(annotated_decl->getDecl())) {
    for (const clang::CXXBaseSpecifier &base : decl->bases()) {
      const clang::TagDecl *base_decl =
          base.getType()->getAsTagDecl()->getDefinition();
      if (base.getAccessSpecifier() != clang::AS_public ||
          annotated_decl->hide_base.count(base_decl) != 0 ||
          annotated_decl->inline_base.count(base_decl) != 0 ||
          graph.getNode(base_decl) == nullptr)
        continue;
      os << ", " << getFullyQualifiedName(base_decl);
    }
  }

  if (!annotated_decl->holder_type.empty()) {
    os << ", " << annotated_decl->holder_type;
  }

  os << ">";
}

void RecordExposer::handleDeclImpl(llvm::raw_ostream &os,
                                   const clang::NamedDecl *decl,
                                   const AnnotatedNamedDecl *annotation) {
  const auto *record_decl =
      llvm::cast<clang::CXXRecordDecl>(annotated_decl->getDecl());
  const clang::ASTContext &ast_context = decl->getASTContext();
  const auto printing_policy = getPrintingPolicyForExposedNames(ast_context);

  if (const auto *annot =
          llvm::dyn_cast<AnnotatedConstructorDecl>(annotation)) {
    const auto *constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(decl);
    if (constructor->isMoveConstructor() || constructor->isDeleted() ||
        record_decl->isAbstract())
      return;

    if (annot->implicit_conversion) {
      clang::QualType from_qual_type = constructor->getParamDecl(0)->getType();
      clang::QualType to_qual_type = ast_context.getTypeDeclType(record_decl);
      os << "::pybind11::implicitly_convertible<"
         << clang::TypeName::getFullyQualifiedName(from_qual_type, ast_context,
                                                   printing_policy,
                                                   /*WithGlobalNsPrefix=*/true)
         << ", "
         << clang::TypeName::getFullyQualifiedName(to_qual_type, ast_context,
                                                   printing_policy,
                                                   /*WithGlobalNsPrefix=*/true)
         << ">();\n";
    }

    os << "context.def(::pybind11::init<";
    emitParameterTypes(os, constructor);
    os << ">(), ";
    emitStringLiteral(os, getDocstring(constructor));
    emitParameters(os, annot);
    emitPolicies(os, annot);
    os << ");\n";
    return;
  }

  // Operators can be either member functions or free functions in the record's
  // associated namespace (found via ADL).
  if (llvm::isa<AnnotatedFunctionDecl>(annotation)) {
    const auto *function = llvm::cast<clang::FunctionDecl>(decl);
    if (function->isOverloadedOperator() &&
        function->getOverloadedOperator() != clang::OO_Call &&
        !function->isDeleted()) {
      emitOperator(os, function);
      return;
    }
  }

  // If the method should be turned into a property, remember this for later.
  if (const auto *annot = llvm::dyn_cast<AnnotatedMethodDecl>(annotation)) {
    const auto *method = llvm::cast<clang::CXXMethodDecl>(decl);
    if (!annot->getter_for.empty() || !annot->setter_for.empty()) {
      for (auto const &name : annot->getter_for) {
        const clang::CXXMethodDecl *previous =
            std::exchange(properties[name].getter, method);
        if (previous != nullptr) {
          Diagnostics::report(decl,
                              Diagnostics::Kind::PropertyAlreadyDefinedError)
              << name << 0U;
          Diagnostics::report(previous, clang::diag::note_previous_definition);
        }
      }
      for (auto const &name : annot->setter_for) {
        const clang::CXXMethodDecl *previous =
            std::exchange(properties[name].setter, method);
        if (previous != nullptr) {
          Diagnostics::report(decl,
                              Diagnostics::Kind::PropertyAlreadyDefinedError)
              << name << 1U;
          Diagnostics::report(previous, clang::diag::note_previous_definition);
        }
      }
      return;
    }
  }

  if (const auto *annot = llvm::dyn_cast<AnnotatedFieldOrVarDecl>(annotation)) {
    if (annot->manual_bindings != nullptr) {
      assert(!annot->postamble && "postamble only allowed in global scope");
      emitManualBindings(os, ast_context, annot->manual_bindings);
      return;
    }
    clang::QualType type = llvm::cast<clang::ValueDecl>(decl)->getType();
    bool readonly = type.isConstQualified() || annot->readonly;
    os << (readonly ? "context.def_readonly" : "context.def_readwrite");
    os << (llvm::isa<clang::VarDecl>(decl) ? "_static(" : "(");
    emitStringLiteral(os, annotation->getSpelling());
    os << ", &::";
    decl->printQualifiedName(os, printing_policy);
    os << ");\n";
    return;
  }

  // Fall back to generic implementation.
  DeclContextExposer::handleDeclImpl(os, decl, annotation);
}
