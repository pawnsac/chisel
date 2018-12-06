#include "GlobalReduction.h"

#include <spdlog/spdlog.h>

#include "FileManager.h"
#include "OptionManager.h"
#include "Profiler.h"

void GlobalReduction::Initialize(clang::ASTContext &Ctx) {
  Reduction::Initialize(Ctx);
  CollectionVisitor = new GlobalElementCollectionVisitor(this);
}

bool GlobalReduction::HandleTopLevelDecl(clang::DeclGroupRef D) {
  for (clang::DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    CollectionVisitor->TraverseDecl(*I);
  }
  return true;
}

void GlobalReduction::HandleTranslationUnit(clang::ASTContext &Ctx) {
  std::vector<llvm::PointerUnion<clang::Decl *, clang::Stmt *>> elements;
  elements.resize(Decls.size());
  std::transform(Decls.begin(), Decls.end(), elements.begin(), CastElement);
  doDeltaDebugging(elements);
}

DDElement GlobalReduction::CastElement(clang::Decl *D) { return D; }

bool GlobalReduction::callOracle() {
  Profiler::GetInstance()->incrementGlobalReductionCounter();

  if (Reduction::callOracle()) {
    Profiler::GetInstance()->incrementSuccessfulGlobalReductionCounter();
    FileManager::GetInstance()->saveTemp("global", true);
    return true;
  } else {
    FileManager::GetInstance()->saveTemp("global", false);
    return false;
  }
}

bool GlobalReduction::test(std::vector<DDElement> &ToBeRemoved) {
  const clang::SourceManager *SM = &Context->getSourceManager();
  std::vector<clang::SourceRange> Ranges;
  std::vector<llvm::StringRef> Reverts;

  for (auto const &D : ToBeRemoved) {
    clang::SourceLocation Start =
        D.get<clang::Decl *>()->getSourceRange().getBegin();
    clang::SourceLocation End;
    clang::FunctionDecl *FD =
        llvm::dyn_cast<clang::FunctionDecl>(D.get<clang::Decl *>());
    if (FD && FD->isThisDeclarationADefinition()) {
      End = FD->getSourceRange().getEnd().getLocWithOffset(1);
    } else if (clang::EmptyDecl *ED =
                   llvm::dyn_cast<clang::EmptyDecl>(D.get<clang::Decl *>())) {
      End = ED->getSourceRange().getEnd().getLocWithOffset(1);
    } else {
      End = getEndLocationUntil(D.get<clang::Decl *>()->getSourceRange(), ';')
                .getLocWithOffset(1);
    }
    const clang::SourceRange Range(Start, End);
    Ranges.emplace_back(Range);
    llvm::StringRef CurrRevert = getSourceText(Range);
    Reverts.emplace_back(CurrRevert);
    removeSourceText(Range);
  }

  TheRewriter.overwriteChangedFiles();

  if (callOracle()) {
    return true;
  } else {
    for (int i = 0; i < Reverts.size(); i++) {
      TheRewriter.ReplaceText(Ranges[i], Reverts[i]);
    }
    TheRewriter.overwriteChangedFiles();
    return false;
  }
}

std::vector<DDElementVector>
GlobalReduction::refineChunks(std::vector<DDElementVector> &Chunks) {
  std::vector<DDElementVector> result;
  for (auto const &Chunk : Chunks) {
    if (std::all_of(std::begin(Chunk), std::end(Chunk), [&](DDElement i) {
          return UseInfo[i.get<clang::Decl *>()].size() == 0;
        }))
      result.emplace_back(Chunk);
  }
  return result;
}

void GlobalElementCollectionVisitor::findAndInsert(clang::Decl *D,
                                                   clang::DeclRefExpr *DRE) {
  if (Consumer->UseInfo.find(D) != Consumer->UseInfo.end()) {
    std::vector<clang::DeclRefExpr *> &Uses = Consumer->UseInfo[D];
    Uses.emplace_back(DRE);
  } else {
    std::vector<clang::DeclRefExpr *> Uses;
    Uses.emplace_back(DRE);
    Consumer->UseInfo.insert(std::make_pair(D, Uses));
  }
}

bool GlobalElementCollectionVisitor::VisitDeclRefExpr(clang::DeclRefExpr *DRE) {
  if (clang::FunctionDecl *FD =
          llvm::dyn_cast<clang::FunctionDecl>(DRE->getDecl())) {
    if (FD->isThisDeclarationADefinition()) {
      Consumer->UseInfo[FD].emplace_back(DRE);
    } else {
      Consumer->UseInfo[DRE->getDecl()].emplace_back(DRE);
    }
  }
  return true;
}

bool GlobalElementCollectionVisitor::VisitFunctionDecl(
    clang::FunctionDecl *FD) {
  spdlog::get("Logger")->debug("Visit Function Decl: {}",
                               FD->getNameInfo().getAsString());
  // hard rule : must contain main()
  if (!FD->isMain()) {
    Consumer->Decls.emplace_back(FD);
  }
  return true;
}

bool GlobalElementCollectionVisitor::VisitVarDecl(clang::VarDecl *VD) {
  if (VD->hasGlobalStorage()) {
    spdlog::get("Logger")->debug("Visit Var Decl: {}", VD->getNameAsString());
    Consumer->Decls.emplace_back(VD);
  }
  return true;
}

bool GlobalElementCollectionVisitor::VisitRecordDecl(clang::RecordDecl *RD) {
  spdlog::get("Logger")->debug("Visit Record Decl: {}", RD->getNameAsString());
  Consumer->Decls.emplace_back(RD);
  return true;
}

bool GlobalElementCollectionVisitor::VisitTypedefDecl(clang::TypedefDecl *TD) {
  spdlog::get("Logger")->debug("Visit Typedef Decl: {}", TD->getNameAsString());
  assert(Consumer != nullptr);
  Consumer->Decls.emplace_back(TD);
  return true;
}

bool GlobalElementCollectionVisitor::VisitEnumDecl(clang::EnumDecl *ED) {
  spdlog::get("Logger")->debug("Visit Enum Decl: {}", ED->getNameAsString());
  Consumer->Decls.emplace_back(ED);
  return true;
}

bool GlobalElementCollectionVisitor::VisitEmptyDecl(clang::EmptyDecl *ED) {
  spdlog::get("Logger")->debug("Visit Empty Decl");
  Consumer->Decls.emplace_back(ED);
  return true;
}
